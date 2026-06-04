# syntax=docker/dockerfile:1.7
# Force amd64 build (Rinha runner is amd64-native).

FROM --platform=linux/amd64 alpine:3.20 AS build

RUN apk add --no-cache build-base linux-headers musl-dev

WORKDIR /src
COPY Makefile tree.h parse.h parse.c server.c ./
# Alpine's system gcc is musl-based; -static produces a musl-linked binary.
RUN make CC=gcc CFLAGS="-O3 -flto -march=haswell -Wall -Wextra -Wno-unused-parameter" \
        LDFLAGS="-static -s" fraud-api-static

FROM --platform=linux/amd64 scratch

WORKDIR /app
COPY --from=build /src/fraud-api-static /app/fraud-api

EXPOSE 8080

CMD ["/app/fraud-api"]
