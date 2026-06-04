// test_runner.c - valida parse + classify contra /tmp/rinha-official/test/test-data.json
#include "parse.h"
#include "tree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// procura substring em região [start, end)
static const char *find_in(const char *start, const char *end, const char *needle, size_t nlen) {
    for (const char *p = start; p + nlen <= end; p++) {
        if (memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

static int classify(const double q[VEC_DIM]) {
    int32_t node = 0;
    while (1) {
        int8_t feat = tree_features[node];
        double thr = tree_thresholds[node];
        int32_t l = tree_left[node];
        if (l < 0) return (int)tree_values[node];
        int32_t r = tree_right[node];
        node = (q[feat] <= thr) ? l : r;
        if (node < 0 || node >= TREE_NODES) return 0;
    }
}

int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "/tmp/rinha-official/test/test-data.json";
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc((size_t)sz + 1);
    if (!data) { perror("malloc"); return 1; }
    fread(data, 1, (size_t)sz, f);
    data[sz] = 0;
    fclose(f);
    const char *end = data + sz;

    int total = 0, tp = 0, tn = 0, fp = 0, fn = 0, err = 0;
    const char *p = data;
    while (1) {
        const char *req_key = find_in(p, end, "\"request\":", 10);
        if (!req_key) break;
        const char *req_start = skip_ws(req_key + 10);
        if (req_start >= end || *req_start != '{') break;
        int depth = 0;
        const char *req_end = req_start;
        while (req_end < end) {
            if (*req_end == '{') depth++;
            else if (*req_end == '}') { depth--; if (depth == 0) { req_end++; break; } }
            req_end++;
        }
        if (depth != 0) break;
        const char *ea_key = find_in(req_end, end, "\"expected_approved\":", 19);
        if (!ea_key) break;
        const char *ea = skip_ws(ea_key + 20);
        bool expected_approved = (ea < end - 4 && strncmp(ea, "true", 4) == 0);
        bool expected_fraud = !expected_approved;

        size_t reqlen = (size_t)(req_end - req_start);
        double q[VEC_DIM];
        bool ok = parse_fraud_request(req_start, reqlen, q);
        total++;
        if (!ok) { err++; }
        else {
            int got_fraud = classify(q);
            if (got_fraud == expected_fraud) {
                if (expected_fraud) tp++; else tn++;
            } else {
                if (expected_fraud) fn++; else fp++;
            }
            if ((got_fraud != expected_fraud)) {
                printf("MISMATCH #%d exp_approved=%d got_fraud=%d q=[%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f]\n",
                       total, expected_approved ? 1 : 0, got_fraud,
                       q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13]);
            }
        }
        p = req_end;
    }

    free(data);
    printf("{\"total\":%d,\"tp\":%d,\"tn\":%d,\"fp\":%d,\"fn\":%d,\"err\":%d}\n",
           total, tp, tn, fp, fn, err);
    return 0;
}
