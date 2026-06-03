#!/usr/bin/env python3
"""Build the official-style results.json from a k6 summary export + counters."""
from __future__ import annotations
import json
import os
import sys
import math

EXPECTED = {
    "total": 54100,
    "fraud_count": 23959,
    "legit_count": 30141,
    "fraud_rate": 0.4429,
    "legit_rate": 0.5571,
    "edge_case_count": 645,
    "edge_case_rate": 0.0119,
}

K = 1000
T_MAX_MS = 1000
P99_MIN_MS = 1
P99_MAX_MS = 2000
EPSILON_MIN = 0.001
BETA = 300
TX_CORTE = 0.15


def main() -> int:
    summary_path = sys.argv[1] if len(sys.argv) > 1 else "test/k6-summary-export.json"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "test/results.json"

    with open(summary_path) as f:
        d = json.load(f)
    m = d.get("metrics", {})

    def counter(name):
        v = m.get(name, {})
        return v.get("values", {}).get("count") or v.get("count") or 0

    tp = counter("tp_count")
    tn = counter("tn_count")
    fp = counter("fp_count")
    fn = counter("fn_count")
    errs = counter("error_count")
    http = m.get("http_req_duration", {})
    p99 = http.get("values", {}).get("p(99)") or http.get("p(99)") or 0

    N = tp + tn + fp + fn + errs
    E = (fp * 1) + (fn * 3) + (errs * 5)
    failures = fp + fn + errs
    epsilon = E / N if N > 0 else 0
    failure_rate = failures / N if N > 0 else 0

    if p99 <= 0:
        p99_score = 0
    elif p99 > P99_MAX_MS:
        p99_score = -3000
    else:
        p99_score = K * math.log10(T_MAX_MS / max(p99, P99_MIN_MS))

    if failure_rate > TX_CORTE:
        det_score = -3000
        rate_component = 0
        absolute_penalty = 0
        cut = True
    else:
        rate_component = K * math.log10(1 / max(epsilon, EPSILON_MIN))
        absolute_penalty = -BETA * math.log10(1 + E)
        det_score = rate_component + absolute_penalty
        cut = False

    final = p99_score + det_score

    result = {
        "expected": EXPECTED,
        "p99": p99,
        "scoring": {
            "breakdown": {
                "false_positive_detections": fp,
                "false_negative_detections": fn,
                "true_positive_detections": tp,
                "true_negative_detections": tn,
                "http_errors": errs,
            },
            "failure_rate": round(failure_rate, 6),
            "weighted_errors_E": E,
            "error_rate_epsilon": epsilon,
            "p99_score": {"value": round(p99_score, 2), "cut_triggered": p99 > P99_MAX_MS},
            "detection_score": {
                "value": round(det_score, 2),
                "rate_component": round(rate_component, 2) if not cut else None,
                "absolute_penalty": round(absolute_penalty, 2) if not cut else None,
                "cut_triggered": cut,
            },
            "final_score": round(final, 2),
        },
    }
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
