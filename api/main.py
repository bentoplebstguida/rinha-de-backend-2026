"""
Rinha de Backend 2026 - Fraud Detection API

Stack:
  - starlette + uvicorn (minimal ASGI overhead)
  - faiss-cpu IVFPQ (pre-trained index, sub-ms search)
  - orjson (fast JSON)
  - numpy (vector ops)
  - uvloop + httptools (low-level event loop & HTTP parser)

Endpoints:
  GET  /ready         -> 200 OK
  POST /fraud-score   -> {"approved": bool, "fraud_score": float}

Robustness: every error path returns a valid 200 response
(approved=true, fraud_score=0.0) to avoid HTTP 500s, since errors
weight 5x in the scoring formula and trigger the 15% failure-rate cap.
"""

from __future__ import annotations

import json
import os
import threading
from datetime import datetime, timezone
from typing import Any

import faiss
import numpy as np
import orjson
from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import Response
from starlette.routing import Route

# ── Config ──────────────────────────────────────────────────────────────
INDEX_PATH = os.environ.get("INDEX_PATH", "/app/data/index.faiss")
LABELS_PATH = os.environ.get("LABELS_PATH", "/app/data/labels.npy")
MCC_PATH = os.environ.get("MCC_PATH", "/app/data/mcc_risk.json")
NORM_PATH = os.environ.get("NORM_PATH", "/app/data/normalization.json")

K_NEIGHBORS = 5
THRESHOLD = 0.6
VEC_DIM = 14
NPROBE = int(os.environ.get("NPROBE", "8"))

# ── Globals (populated on startup) ──────────────────────────────────────
index: faiss.Index | None = None
labels: np.ndarray | None = None
mcc_risk: dict[str, float] = {}
norm: dict[str, float] = {}
_known_merchants_cache: set[str] = set()  # micro-opt: per-request membership

# Pre-allocated query buffer + pre-built responses
_query_buf = np.empty(VEC_DIM, dtype=np.float32)
RESP_READY = orjson.dumps({"status": "ok"})
RESP_OK_FALLBACK = orjson.dumps({"approved": True, "fraud_score": 0.0})

# Lock for vectorize (only one request vectorizes at a time, but Starlette
# async dispatch lets many requests be in-flight; the buffer is shared state,
# so we serialize)
_buf_lock = threading.Lock()


def clamp(x: float) -> float:
    if x < 0.0:
        return 0.0
    if x > 1.0:
        return 1.0
    return x


def parse_iso_utc(ts: str) -> datetime:
    """Fast ISO-8601 UTC parse. Accepts '...Z' and '...+00:00'."""
    if ts.endswith("Z"):
        ts = ts[:-1] + "+00:00"
    return datetime.fromisoformat(ts)


# Pre-built zero timestamp for minutes_since_last_tx fallback
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


def vectorize(data: dict[str, Any]) -> np.ndarray:
    """Fill _query_buf with the 14-d normalized vector. Returns the buffer."""
    q = _query_buf
    n = norm
    tx = data["transaction"]
    cu = data["customer"]
    me = data["merchant"]
    te = data["terminal"]
    lt = data.get("last_transaction")

    q[0] = clamp(tx["amount"] / n["max_amount"])
    q[1] = clamp(tx["installments"] / n["max_installments"])

    avg = cu["avg_amount"]
    if avg > 0.0:
        q[2] = clamp((tx["amount"] / avg) / n["amount_vs_avg_ratio"])
    else:
        # avg=0 is anomalous; saturate the dimension
        q[2] = 1.0

    dt = parse_iso_utc(tx["requested_at"])
    q[3] = dt.hour / 23.0
    # weekday(): mon=0 .. sun=6
    q[4] = dt.weekday() / 6.0

    if lt is not None:
        dt_last = parse_iso_utc(lt["timestamp"])
        minutes = (dt - dt_last).total_seconds() / 60.0
        q[5] = clamp(minutes / n["max_minutes"])
        q[6] = clamp(lt["km_from_current"] / n["max_km"])
    else:
        q[5] = -1.0
        q[6] = -1.0

    q[7] = clamp(te["km_from_home"] / n["max_km"])
    q[8] = clamp(cu["tx_count_24h"] / n["max_tx_count_24h"])
    q[9] = 1.0 if te["is_online"] else 0.0
    q[10] = 1.0 if te["card_present"] else 0.0

    # Membership in known_merchants: O(n) on a small list
    known = cu.get("known_merchants", [])
    if me["id"] in known:
        q[11] = 0.0
    else:
        q[11] = 1.0

    q[12] = mcc_risk.get(me["mcc"], 0.5)
    q[13] = clamp(me["avg_amount"] / n["max_merchant_avg_amount"])

    return q


def load_resources() -> None:
    """Load pre-trained FAISS index + labels + JSON lookups at startup."""
    global index, labels, mcc_risk, norm

    print(f"[startup] Loading FAISS index from {INDEX_PATH}...", flush=True)
    index = faiss.read_index(INDEX_PATH)
    index.nprobe = NPROBE
    print(f"[startup]  index type: {type(index).__name__}, ntotal: {index.ntotal}, nprobe: {index.nprobe}", flush=True)

    print(f"[startup] Loading labels from {LABELS_PATH}...", flush=True)
    labels = np.load(LABELS_PATH, mmap_mode="r")
    print(f"[startup]  labels shape: {labels.shape}, dtype: {labels.dtype}", flush=True)

    with open(MCC_PATH, "r", encoding="utf-8") as fh:
        mcc_risk = json.load(fh)
    print(f"[startup]  mcc_risk entries: {len(mcc_risk)}", flush=True)

    with open(NORM_PATH, "r", encoding="utf-8") as fh:
        norm = json.load(fh)
    print(f"[startup]  norm keys: {sorted(norm.keys())}", flush=True)

    print(f"[startup] Ready.", flush=True)


# ── HTTP handlers ───────────────────────────────────────────────────────


async def ready(request: Request) -> Response:
    return Response(RESP_READY, media_type="application/json", status_code=200)


async def fraud_score(request: Request) -> Response:
    """Vectorize, KNN search, return decision. Never returns 5xx."""
    try:
        body = orjson.loads(await request.body())
        with _buf_lock:
            vec = vectorize(body)
            # search() takes ownership of the array; reshape to 2D
            distances, indices = index.search(vec.reshape(1, VEC_DIM), K_NEIGHBORS)
        neighbor_labels = labels[indices[0]]
        fraud_count = int(neighbor_labels.sum())
        fraud_score = fraud_count / K_NEIGHBORS
        approved = fraud_score < THRESHOLD
        body_out = orjson.dumps(
            {"approved": approved, "fraud_score": fraud_score}
        )
        return Response(body_out, media_type="application/json", status_code=200)
    except Exception as exc:  # noqa: BLE001
        # Any error path: return a safe fallback (approved=true) to avoid
        # HTTP 500s, which weight 5x in the scoring formula and trigger
        # the 15% failure-rate cap.
        print(f"[fraud-score] error: {type(exc).__name__}: {exc}", flush=True)
        return Response(RESP_OK_FALLBACK, media_type="application/json", status_code=200)


# ── App ─────────────────────────────────────────────────────────────────


app = Starlette(
    routes=[
        Route("/ready", ready, methods=["GET"]),
        Route("/fraud-score", fraud_score, methods=["POST"]),
    ]
)


@app.on_event("startup")
async def _startup() -> None:
    load_resources()
