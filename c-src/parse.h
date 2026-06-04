// parse.h - JSON 1-pass parser for Rinha 2026 fraud-score endpoint.
// Zero-allocation. Operates directly on the read buffer.
#ifndef RINHA_PARSE_H
#define RINHA_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tree.h"

typedef struct {
    // input
    const char *buf;
    size_t len;
    // output vector (14 features)
    double q[VEC_DIM];
    // parsed status
    bool ok;
} parser_t;

// Entry point. Reads a JSON request body from buf, fills q.
// Returns true on success. On failure, q is left undefined.
bool parse_fraud_request(const char *buf, size_t len, double out[VEC_DIM]);

#endif // RINHA_PARSE_H
