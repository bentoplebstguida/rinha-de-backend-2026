#!/usr/bin/env python3
"""
Rinha 2026 accuracy test: hits the live API with each entry from
test/test-data.json and compares the response to expected_approved.

Outputs a results.json shaped like the official engine's output.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any

DEFAULT_URL = "http://localhost:19999"


def post_fraud(url: str, payload: dict[str, Any], timeout: float) -> tuple[int, dict[str, Any] | None, str | None]:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"{url}/fraud-score",
        data=data,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    t0 = time.perf_counter_ns()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            latency_ms = (time.perf_counter_ns() - t0) / 1_000_000
            return resp.status, json.loads(body), body, latency_ms
    except urllib.error.HTTPError as e:
        latency_ms = (time.perf_counter_ns() - t0) / 1_000_000
        return e.code, None, e.read().decode("utf-8", errors="replace"), latency_ms
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
        latency_ms = (time.perf_counter_ns() - t0) / 1_000_000
        return 0, None, str(e), latency_ms


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="/tmp/rinha-test-data.json")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--workers", type=int, default=32)
    ap.add_argument("--limit", type=int, default=0, help="0 = all")
    ap.add_argument("--out", default="/tmp/rinha-arm64-build/accuracy.json")
    args = ap.parse_args()

    with open(args.data) as f:
        data = json.load(f)
    entries = data["entries"]
    if args.limit:
        entries = entries[: args.limit]

    n = len(entries)
    print(f"running accuracy test: n={n} url={args.url} workers={args.workers}", flush=True)

    latencies: list[float] = []
    tp = tn = fp = fn = err = 0
    started = time.time()

    def hit(idx: int, e: dict[str, Any]) -> tuple[int, float, dict[str, Any]]:
        status, body, raw, lat = post_fraud(args.url, e["request"], args.timeout)
        return status, lat, {"idx": idx, "status": status, "body": body, "raw": raw, "expected_approved": e["expected_approved"], "expected_fraud_score": e["expected_fraud_score"]}

    results: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(hit, i, e) for i, e in enumerate(entries)]
        done = 0
        for fut in as_completed(futs):
            status, lat, r = fut.result()
            latencies.append(lat)
            results.append(r)
            if status != 200 or r["body"] is None:
                err += 1
            else:
                got = bool(r["body"].get("approved"))
                want = bool(r["expected_approved"])
                if got and want:
                    tn += 1
                elif (not got) and (not want):
                    tp += 1
                elif got and (not want):
                    fn += 1
                else:
                    fp += 1
            done += 1
            if done % 1000 == 0:
                print(f"  {done}/{n} ({(done/n)*100:.1f}%) tp={tp} tn={tn} fp={fp} fn={fn} err={err}", flush=True)

    elapsed = time.time() - started
    latencies_sorted = sorted(latencies)
    def pct(p: float) -> float:
        if not latencies_sorted:
            return 0.0
        k = max(0, min(len(latencies_sorted) - 1, int(round((p / 100.0) * (len(latencies_sorted) - 1)))))
        return latencies_sorted[k]

    total_responses = tp + tn + fp + fn + err
    summary = {
        "n": n,
        "elapsed_seconds": round(elapsed, 2),
        "throughput_rps": round(n / elapsed, 1) if elapsed > 0 else 0,
        "confusion": {
            "tp": tp, "tn": tn, "fp": fp, "fn": fn, "err": err,
        },
        "failure_rate": round((fp + fn + err) / total_responses, 6) if total_responses else 0,
        "weighted_errors_E": fp + 3 * fn + 5 * err,
        "p50_ms": round(pct(50), 3),
        "p90_ms": round(pct(90), 3),
        "p95_ms": round(pct(95), 3),
        "p99_ms": round(pct(99), 3),
        "p100_ms": round(pct(100), 3),
    }

    out = {
        "summary": summary,
        "expected": data.get("stats", {}),
        "checksum": data.get("references_checksum_sha256"),
        "mismatches": [r for r in results if r["body"] is not None and bool(r["body"].get("approved")) != bool(r["expected_approved"])],
    }
    with open(args.out, "w") as f:
        json.dump(out, f, indent=2)

    print(json.dumps(summary, indent=2), flush=True)
    return 0 if (fp == 0 and fn == 0 and err == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
