#!/usr/bin/env python3
"""
Preprocess references.json.gz into a pre-trained FAISS IVFPQ index + labels.npy.

Output:
  - vectors.npy   (float32, Nx14) — raw vectors (kept for debug, not needed at runtime)
  - labels.npy    (uint8,  N)     — 1 = fraud, 0 = legit
  - index.faiss                 — pre-trained FAISS IndexIVFPQ

Usage:
  python preprocess.py references.json.gz /app/data
"""

from __future__ import annotations

import argparse
import gc
import gzip
import os
import sys
import time

import faiss
import ijson
import numpy as np

VEC_DIM = 14

# FAISS IVFPQ params — chosen for memory budget (~50MB index) and recall.
# M must divide VEC_DIM exactly; 14 dimensions -> 7 subquantizers.
NLIST = 1024
M = 7
NBITS = 8


def preprocess(input_path: str, output_dir: str) -> None:
    os.makedirs(output_dir, exist_ok=True)

    print(f"[preprocess] Counting records in {input_path}...", flush=True)
    t0 = time.time()
    with gzip.open(input_path, "rt", encoding="utf-8") as fh:
        count = sum(1 for _ in ijson.items(fh, "item"))
    print(f"[preprocess] {count:,} records in {time.time() - t0:.1f}s", flush=True)

    # Pre-allocate
    vectors = np.empty((count, VEC_DIM), dtype=np.float32)
    labels = np.empty(count, dtype=np.uint8)

    print(f"[preprocess] Loading and vectorizing...", flush=True)
    t0 = time.time()
    with gzip.open(input_path, "rt", encoding="utf-8") as fh:
        for i, item in enumerate(ijson.items(fh, "item")):
            vectors[i] = item["vector"]
            labels[i] = 1 if item["label"] == "fraud" else 0
            if (i + 1) % 500_000 == 0:
                print(f"[preprocess]  {i+1:,}/{count:,}", flush=True)
    print(f"[preprocess] Loaded in {time.time() - t0:.1f}s", flush=True)

    # Save labels (always needed at runtime for fraud lookup)
    labels_path = os.path.join(output_dir, "labels.npy")
    np.save(labels_path, labels)
    print(
        f"[preprocess] Saved {labels_path} ({os.path.getsize(labels_path)/1024/1024:.1f} MB)",
        flush=True,
    )

    # Save raw vectors (kept for debug; runtime only needs index + labels)
    vectors_path = os.path.join(output_dir, "vectors.npy")
    np.save(vectors_path, vectors)
    print(
        f"[preprocess] Saved {vectors_path} ({os.path.getsize(vectors_path)/1024/1024:.1f} MB)",
        flush=True,
    )

    frauds = int(labels.sum())
    legit = count - frauds
    print(
        f"[preprocess] Labels: {frauds:,} fraud / {legit:,} legit / {count:,} total",
        flush=True,
    )

    # Free memory before training
    del vectors
    gc.collect()

    # Train FAISS IVFPQ index
    print(
        f"[preprocess] Training FAISS IVFPQ (nlist={NLIST}, M={M}, nbits={NBITS})...",
        flush=True,
    )
    t0 = time.time()
    quantizer = faiss.IndexFlatL2(VEC_DIM)
    index = faiss.IndexIVFPQ(quantizer, VEC_DIM, NLIST, M, NBITS, faiss.METRIC_L2)

    # Reload vectors for training/add
    vectors = np.load(vectors_path)
    print(f"[preprocess] Training on {len(vectors):,} vectors...", flush=True)
    index.train(vectors)
    print(f"[preprocess] Adding vectors to index...", flush=True)
    index.add(vectors)
    print(f"[preprocess] Index built in {time.time() - t0:.1f}s", flush=True)

    # Set sane default nprobe
    index.nprobe = 8

    index_path = os.path.join(output_dir, "index.faiss")
    faiss.write_index(index, index_path)
    print(
        f"[preprocess] Saved {index_path} ({os.path.getsize(index_path)/1024/1024:.1f} MB)",
        flush=True,
    )

    print(f"[preprocess] Done.", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Preprocess references.json.gz into FAISS IVFPQ + labels"
    )
    parser.add_argument("input", help="Path to references.json.gz")
    parser.add_argument("output_dir", help="Output directory for index + npy")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"[preprocess] Error: not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    preprocess(args.input, args.output_dir)


if __name__ == "__main__":
    main()
