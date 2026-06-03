#!/usr/bin/env python3
"""Compare bench results from two variants."""
from __future__ import annotations
import json
import os
import sys


def load(path: str):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def fmt_us(v):
    if v is None: return "n/a"
    return f"{v:.1f}us"


def safe(metric_dict, key, sub):
    m = metric_dict.get(key)
    if not m: return None
    return m.get("values", {}).get(sub)


def main() -> int:
    rows = [
        ("A-LB-Go", "A-lbgo-summary.json", "A-lbgo-accuracy.json"),
        ("B-nginx", "B-nginx-summary.json", "B-nginx-accuracy.json"),
    ]
    for label, sp, ap in rows:
        d = load(sp)
        if not d:
            print(f"{label}: NO DATA")
            continue
        m = d.get("metrics", {})
        drops = safe(m, "dropped_iterations", "count") or 0
        print(
            f"{label} k6: p50={fmt_us(safe(m, 'http_req_duration', 'p(50)'))} "
            f"p90={fmt_us(safe(m, 'http_req_duration', 'p(90)'))} "
            f"p95={fmt_us(safe(m, 'http_req_duration', 'p(95)'))} "
            f"p99={fmt_us(safe(m, 'http_req_duration', 'p(99)'))} "
            f"max={fmt_us(safe(m, 'http_req_duration', 'max'))} "
            f"rps={(safe(m, 'http_reqs', 'rate') or 0):.1f} "
            f"drops={drops}"
        )
    for label, _, ap in rows:
        d = load(ap)
        if not d:
            print(f"{label} accuracy: NO DATA")
            continue
        s = d["summary"]
        c = s["confusion"]
        print(
            f"{label} accuracy: tp={c['tp']} tn={c['tn']} fp={c['fp']} fn={c['fn']} err={c['err']} "
            f"p99={s['p99_ms']:.2f}ms"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
