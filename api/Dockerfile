# syntax=docker/dockerfile:1.7
# Force amd64 for the rinha runner (Mac Mini Late 2014).
# `docker buildx build --platform linux/amd64` is also fine.

# ── Stage 1: Builder — download data, train FAISS IVFPQ index ───────────
FROM --platform=linux/amd64 python:3.12-slim AS builder

WORKDIR /build

# Single-threaded BLAS for the build phase too (predictable, faster on small CPU)
ENV OMP_NUM_THREADS=1 \
    MKL_NUM_THREADS=1 \
    OPENBLAS_NUM_THREADS=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends wget ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir \
    "numpy>=2.2.0,<2.3.0" \
    "faiss-cpu>=1.10.0,<1.11.0" \
    "ijson>=3.3.0,<3.4.0"

# Download reference data files from the official repo
RUN mkdir -p /build/data
ADD https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/resources/mcc_risk.json /build/data/
ADD https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/main/resources/normalization.json /build/data/
ADD https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources/references.json.gz /build/data/

# Preprocess: references.json.gz → labels.npy + vectors.npy + index.faiss (pre-trained IVFPQ)
COPY preprocess.py /build/
RUN python /build/preprocess.py /build/data/references.json.gz /build/data \
    && rm -f /build/data/references.json.gz /build/data/vectors.npy


# ── Stage 2: Runtime — minimal image, no build tools ───────────────────
FROM --platform=linux/amd64 python:3.12-slim

WORKDIR /app

# Single-threaded BLAS at runtime (env from compose also sets these; this is the safety net)
ENV OMP_NUM_THREADS=1 \
    MKL_NUM_THREADS=1 \
    OPENBLAS_NUM_THREADS=1

# Runtime dependencies only (no ijson, no wget)
RUN pip install --no-cache-dir \
    "starlette>=0.41.0,<0.42.0" \
    "uvicorn[standard]>=0.32.0,<0.33.0" \
    "numpy>=2.2.0,<2.3.0" \
    "faiss-cpu>=1.10.0,<1.11.0" \
    "orjson>=3.10.0,<3.11.0"

# Copy app code
COPY main.py /app/

# Copy pre-built index + labels + JSON lookups from builder
COPY --from=builder /build/data/ /app/data/

EXPOSE 8080

# Minimal uvicorn: 1 worker, uvloop, httptools, no access log
CMD ["uvicorn", "main:app", \
     "--host", "0.0.0.0", \
     "--port", "8080", \
     "--loop", "uvloop", \
     "--http", "httptools", \
     "--workers", "1", \
     "--no-access-log", \
     "--log-level", "warning"]
