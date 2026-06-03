# Rinha de Backend 2026 — Fraud Detection API

API de detecção de fraude por busca vetorial (k-NN) usando **starlette + uvicorn + FAISS IVFPQ**, containerizada em 2 instâncias atrás de **nginx** (load balancer).

## Stack

- **Python 3.12** + **starlette** (ASGI puro, sem overhead de Pydantic) + **uvicorn** (uvloop + httptools)
- **FAISS** `IndexIVFPQ` (nlist=1024, M=8, nbits=8, nprobe=8) — pré-treinado no build
- **numpy** (vetorização) + **orjson** (parse JSON)
- **nginx:alpine** como load balancer round-robin

## Arquitetura

```
nginx:9999 (load balancer)
    ├── api1:8080 (starlette + uvicorn + FAISS IVFPQ)
    └── api2:8080 (starlette + uvicorn + FAISS IVFPQ)
```

### Otimizações

- **Índice pré-treinado no build**: o `IndexIVFPQ` é treinado uma vez no `docker build` (Stage 1) e só carregado em runtime via `faiss.read_index`. O `/ready` responde em < 1s.
- **Memória compacta**: IVFPQ comprime os 3M vetores para ~50MB no índice, ~5MB de labels. Cabe folgada em 140MB.
- **Buffer de query pré-alocado**: vetor de 14 floats reaproveitado entre requests (com lock).
- **Single-thread BLAS**: `OMP_NUM_THREADS=1` evita contenção em CPU limitada.
- **Fallback robusto**: qualquer erro de parsing/KNN devolve `{"approved": true, "fraud_score": 0.0}` (HTTP 200). HTTP 500 pesa 5× na fórmula do score e dispara corte de 15% — então vale um FP de vez em quando pra não errar tudo.

## API

| Endpoint       | Método | Descrição                                  |
| -------------- | ------ | ------------------------------------------ |
| `/ready`       | GET    | Health check — `200 OK` quando pronto      |
| `/fraud-score` | POST   | Recebe transação, retorna decisão          |

Resposta:
```json
{ "approved": true, "fraud_score": 0.0 }
```

## Como executar localmente

```bash
docker compose up --build
```

(O `build` baixa `references.json.gz` (~284MB), treina o índice IVFPQ e gera a imagem. Demora uns 5-10 min no primeiro build.)

Testar:
```bash
curl http://localhost:9999/ready
curl -X POST http://localhost:9999/fraud-score \
  -H "Content-Type: application/json" \
  -d '{
    "id": "tx-test",
    "transaction": {"amount": 100, "installments": 1, "requested_at": "2026-03-11T12:00:00Z"},
    "customer": {"avg_amount": 200, "tx_count_24h": 2, "known_merchants": ["MERC-001"]},
    "merchant": {"id": "MERC-001", "mcc": "5411", "avg_amount": 150},
    "terminal": {"is_online": false, "card_present": true, "km_from_home": 5},
    "last_transaction": null
  }'
```

## Estrutura

```
.
├── api/
│   ├── main.py            # API starlette + uvicorn
│   ├── preprocess.py      # references.json.gz → IVFPQ index + labels.npy
│   ├── requirements.txt
│   └── Dockerfile         # multi-stage: builder treina, runtime serve
├── nginx/
│   └── nginx.conf
├── docker-compose.yml     # build local (com `build:`)
├── docker-compose.submission.yml  # submission branch (com `image:`)
├── info.json
├── LICENSE (MIT)
└── README.md
```

## Submissão

- Branch `main`: código-fonte completo.
- Branch `submission`: apenas `docker-compose.yml` (imagem pública) + `nginx/nginx.conf` + `info.json` + `LICENSE`.

Para rodar o teste, abrir issue com `rinha/test` no repo oficial:
https://github.com/zanfranceschi/rinha-de-backend-2026/issues

## Licença

MIT
