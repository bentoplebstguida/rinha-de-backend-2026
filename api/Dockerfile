# syntax=docker/dockerfile:1.7
# Force amd64 for the rinha runner (Mac Mini Late 2014).

FROM --platform=$BUILDPLATFORM golang:1.23-alpine AS builder

WORKDIR /src
ARG TARGETOS
ARG TARGETARCH
COPY go.mod ./
COPY main.go ./
RUN CGO_ENABLED=0 GOOS=${TARGETOS:-linux} GOARCH=${TARGETARCH:-amd64} go build -ldflags="-s -w" -o /out/fraud-api .

FROM --platform=$TARGETPLATFORM alpine:3.20

WORKDIR /app
COPY --from=builder /out/fraud-api /app/fraud-api

EXPOSE 8080

CMD ["/app/fraud-api"]
