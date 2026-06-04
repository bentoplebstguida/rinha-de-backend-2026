// parse.c - JSON 1-pass parser for Rinha 2026 fraud-score endpoint.
// Mirrors the logic of api/main.go vectorizeBytes(), with zero allocations.
// Order: transaction, customer, merchant, terminal, last_transaction (as in JSON).
#include "parse.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- tiny helpers (no libc allocation) --------------------------------------

// Find first occurrence of needle in haystack starting at start.
// Returns index relative to haystack (NOT start), or -1.
static long find_from(const char *hay, size_t hay_len, long start, const char *needle, size_t needle_len) {
    if (start < 0 || (size_t)start >= hay_len) return -1;
    if (needle_len == 0) return start;
    if (start + needle_len > hay_len) return -1;
    // memmem-style search, but use memchr+memcmp for portability.
    // Note: haystack is not necessarily NUL-terminated.
    for (size_t i = (size_t)start; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return (long)i;
        }
    }
    return -1;
}

// Skip whitespace starting at i. Returns new index.
static long skip_ws(const char *buf, size_t len, long i) {
    while (i < (long)len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) {
        i++;
    }
    return i;
}

// Find position of byte in buffer starting at start; return index or -1.
static long index_byte(const char *buf, size_t len, long start, char c) {
    for (long i = start; i < (long)len; i++) {
        if (buf[i] == c) return i;
    }
    return -1;
}

// Parse a JSON float starting at *i. Advances *i past the number.
// No scientific notation (matches the dataset).
static double parse_float(const char *buf, size_t len, long *i) {
    bool neg = false;
    if (*i < (long)len && buf[*i] == '-') { neg = true; (*i)++; }
    double val = 0.0;
    double frac = 0.0;
    double div = 1.0;
    bool in_frac = false;
    while (*i < (long)len) {
        char c = buf[*i];
        if (c >= '0' && c <= '9') {
            if (in_frac) {
                div *= 10.0;
                frac += (c - '0') / div;
            } else {
                val = val * 10.0 + (c - '0');
            }
            (*i)++;
        } else if (c == '.' && !in_frac) {
            in_frac = true;
            (*i)++;
        } else {
            break;
        }
    }
    return neg ? -(val + frac) : (val + frac);
}

// Parse a JSON int starting at *i. Advances *i.
static long parse_int(const char *buf, size_t len, long *i) {
    long n = 0;
    while (*i < (long)len && buf[*i] >= '0' && buf[*i] <= '9') {
        n = n * 10 + (buf[*i] - '0');
        (*i)++;
    }
    return n;
}

// Find "key": pattern starting at *i, advance *i past the value position.
// On failure, returns false and leaves *i in an unspecified state.
static bool skip_to_key(const char *buf, size_t len, long *i, const char *key) {
    // Build needle "key":
    size_t kl = strlen(key);
    char needle[64];
    if (kl + 3 > sizeof(needle)) return false;
    needle[0] = '"';
    memcpy(needle + 1, key, kl);
    needle[kl + 1] = '"';
    needle[kl + 2] = ':';
    needle[kl + 3] = '\0';
    size_t nl = kl + 3;
    long pos = find_from(buf, len, *i, needle, nl);
    if (pos < 0) return false;
    *i = pos + nl;
    *i = skip_ws(buf, len, *i);
    return true;
}

// Read a JSON string starting with " and ending with ".
// Returns start index (just past opening "), end index (at closing ").
// Returns false if not well-formed.
static bool read_string(const char *buf, size_t len, long *i, long *str_start, long *str_end) {
    if (*i >= (long)len || buf[*i] != '"') return false;
    (*i)++;
    *str_start = *i;
    long end = index_byte(buf, len, *i, '"');
    if (end < 0) return false;
    *str_end = end;
    *i = end + 1;
    return true;
}

// ---- date helpers (mirror Go's dayOfWeek and epochSecondsBytes) -----------

static int atoi2(const char *b, int i) {
    return (b[i] - '0') * 10 + (b[i + 1] - '0');
}
static int atoi4(const char *b, int i) {
    return (b[i] - '0') * 1000 + (b[i + 1] - '0') * 100 +
           (b[i + 2] - '0') * 10 + (b[i + 3] - '0');
}

static int day_of_week(int y, int m, int d) {
    // Tomohiko Sakamoto's algorithm
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int dow = (y + y/4 - y/100 + y/400 + t[m - 1] + d) % 7;
    // Go returns (dow + 6) % 7 (Monday=0). We use the same.
    return (dow + 6) % 7;
}

static long epoch_seconds(const char *ts) {
    int y = atoi4(ts, 0);
    int mo = atoi2(ts, 5);
    int d = atoi2(ts, 8);
    int h = atoi2(ts, 11);
    int mi = atoi2(ts, 14);
    int s = atoi2(ts, 17);
    // Days since 1970-01-01 (Howard Hinnant's algorithm)
    int era = y / 400;
    if (y < 0 && y % 400 != 0) era--;
    int yoe = y - era * 400;
    int mp = mo;
    if (mp > 2) mp -= 3; else mp += 9;
    int doy = (153 * mp + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((long)era * 146097 + doe - 719468) * 86400 +
           (long)h * 3600 + (long)mi * 60 + (long)s;
}

// ---- mcc risk map (matches Go's mccRisk) ------------------------------------

typedef struct { const char *mcc; double risk; } mcc_entry_t;

// Sparse table; entries must be sorted by mcc for bsearch, but we just linear-scan (small).
static const mcc_entry_t mcc_risks[] = {
    {"4511", 0.35}, {"5311", 0.25}, {"5411", 0.15}, {"5812", 0.30},
    {"5912", 0.20}, {"5944", 0.45}, {"5999", 0.50}, {"7801", 0.80},
    {"7802", 0.75}, {"7995", 0.85},
};
#define MCC_RISK_N (sizeof(mcc_risks) / sizeof(mcc_risks[0]))

static double mcc_risk(const char *mcc, long mcc_len) {
    for (size_t i = 0; i < MCC_RISK_N; i++) {
        if (strlen(mcc_risks[i].mcc) == (size_t)mcc_len &&
            memcmp(mcc_risks[i].mcc, mcc, (size_t)mcc_len) == 0) {
            return mcc_risks[i].risk;
        }
    }
    return 0.5;
}

// ---- helpers used by the Go vectorizeBytes port ------------------------------

static inline double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static inline double round4(double x) {
    return round(x * 10000.0) / 10000.0;
}

// ---- main parse function ----------------------------------------------------

bool parse_fraud_request(const char *buf, size_t len, double out[VEC_DIM]) {
    // Defaults for null last_transaction
    out[5] = -1.0;
    out[6] = -1.0;

    long i = 0;

    // -------- transaction ----------
    if (!skip_to_key(buf, len, &i, "transaction")) { fprintf(stderr, "fail: transaction\n"); return false; }
    if (!skip_to_key(buf, len, &i, "amount")) { fprintf(stderr, "fail: amount at i=%ld\n", i); return false; }
    double amount = parse_float(buf, len, &i);
    out[0] = clamp01(amount / 10000.0);

    if (!skip_to_key(buf, len, &i, "installments")) { fprintf(stderr, "fail: installments i=%ld\n", i); return false; }
    long installments = parse_int(buf, len, &i);
    out[1] = clamp01((double)installments / 12.0);

    if (!skip_to_key(buf, len, &i, "requested_at")) { fprintf(stderr, "fail: requested_at i=%ld\n", i); return false; }
    long req_start, req_end;
    if (!read_string(buf, len, &i, &req_start, &req_end)) { fprintf(stderr, "fail: requested_at string i=%ld\n", i); return false; }
    if (req_end - req_start < 19) { fprintf(stderr, "fail: requested_at short\n"); return false; }
    int y = atoi4(buf, req_start);
    int mo = atoi2(buf, req_start + 5);
    int d = atoi2(buf, req_start + 8);
    int h = atoi2(buf, req_start + 11);
    out[3] = (double)h / 23.0;
    out[4] = (double)day_of_week(y, mo, d) / 6.0;

    // -------- customer ----------
    if (!skip_to_key(buf, len, &i, "customer")) { fprintf(stderr, "fail: customer i=%ld\n", i); return false; }
    if (!skip_to_key(buf, len, &i, "avg_amount")) { fprintf(stderr, "fail: avg_amount i=%ld\n", i); return false; }
    double customer_avg = parse_float(buf, len, &i);
    if (customer_avg > 0) {
        out[2] = clamp01((amount / customer_avg) / 10.0);
    } else {
        out[2] = 1.0;
    }
    if (!skip_to_key(buf, len, &i, "tx_count_24h")) { fprintf(stderr, "fail: tx_count_24h i=%ld\n", i); return false; }
    long tx_count_24h = parse_int(buf, len, &i);
    out[8] = clamp01((double)tx_count_24h / 20.0);

    // Skip known_merchants array (walk until matching ']')
    if (!skip_to_key(buf, len, &i, "known_merchants")) { fprintf(stderr, "fail: known_merchants i=%ld\n", i); return false; }
    int depth = 0;
    while (i < (long)len) {
        if (buf[i] == '[') depth++;
        else if (buf[i] == ']') {
            depth--;
            if (depth == 0) { i++; break; }
        }
        i++;
    }
    if (i >= (long)len) return false;
    if (buf[i] == ',') i++;

    // -------- merchant ----------
    if (!skip_to_key(buf, len, &i, "merchant")) { fprintf(stderr, "fail: merchant i=%ld\n", i); return false; }
    if (!skip_to_key(buf, len, &i, "id")) { fprintf(stderr, "fail: id i=%ld\n", i); return false; }
    long mer_start, mer_end;
    if (!read_string(buf, len, &i, &mer_start, &mer_end)) { fprintf(stderr, "fail: id string i=%ld\n", i); return false; }
    if (!skip_to_key(buf, len, &i, "mcc")) { fprintf(stderr, "fail: mcc i=%ld\n", i); return false; }
    long mcc_start, mcc_end;
    if (!read_string(buf, len, &i, &mcc_start, &mcc_end)) { fprintf(stderr, "fail: mcc string i=%ld\n", i); return false; }
    out[12] = mcc_risk(buf + mcc_start, mcc_end - mcc_start);
    if (!skip_to_key(buf, len, &i, "avg_amount")) { fprintf(stderr, "fail: m.avg_amount i=%ld\n", i); return false; }
    double merchant_avg = parse_float(buf, len, &i);
    out[13] = clamp01(merchant_avg / 10000.0);

    // -------- terminal ----------
    if (!skip_to_key(buf, len, &i, "terminal")) { fprintf(stderr, "fail: terminal i=%ld\n", i); return false; }
    // Order in JSON: is_online, card_present, km_from_home
    if (!skip_to_key(buf, len, &i, "is_online")) { fprintf(stderr, "fail: is_online i=%ld\n", i); return false; }
    if (i < (long)len && buf[i] == 't') {
        out[9] = 1.0;
        i += 4;
    } else {
        out[9] = 0.0;
        i += 5;
    }
    if (!skip_to_key(buf, len, &i, "card_present")) { fprintf(stderr, "fail: card_present i=%ld\n", i); return false; }
    if (i < (long)len && buf[i] == 't') {
        out[10] = 1.0;
        i += 4;
    } else {
        out[10] = 0.0;
        i += 5;
    }
    if (!skip_to_key(buf, len, &i, "km_from_home")) { fprintf(stderr, "fail: km_from_home i=%ld\n", i); return false; }
    double km_from_home = parse_float(buf, len, &i);
    out[7] = clamp01(km_from_home / 1000.0);

    // -------- last_transaction ----------
    if (!skip_to_key(buf, len, &i, "last_transaction")) { fprintf(stderr, "fail: last_transaction i=%ld\n", i); return false; }
    if (i + 4 <= (long)len &&
        buf[i] == 'n' && buf[i+1] == 'u' && buf[i+2] == 'l' && buf[i+3] == 'l') {
        // null — q[5] and q[6] stay -1.0
    } else {
        if (!skip_to_key(buf, len, &i, "timestamp")) { fprintf(stderr, "fail: last.timestamp i=%ld\n", i); return false; }
        long lts_start, lts_end;
        if (!read_string(buf, len, &i, &lts_start, &lts_end)) return false;
        if (lts_end - lts_start < 19) return false;
        long req_epoch = epoch_seconds(buf + req_start);
        long last_epoch = epoch_seconds(buf + lts_start);
        double mins = (double)(req_epoch - last_epoch) / 60.0;
        out[5] = clamp01(mins / 1440.0);
        if (!skip_to_key(buf, len, &i, "km_from_current")) { fprintf(stderr, "fail: last.km_from_current i=%ld\n", i); return false; }
        double km_from_current = parse_float(buf, len, &i);
        out[6] = clamp01(km_from_current / 1000.0);
    }

    // -------- merchant.id in known_merchants ----------
    // Find array bounds: locate "known_merchants":[ and matching ].
    long km_start = find_from(buf, len, 0, "\"known_merchants\":[", 19);
    if (km_start >= 0) {
        // km_start points to the first '"' of "known_merchants":[.
        // The '[' is at km_start + 18. We start scanning *inside* the array,
        // so depth must begin at 1 and we break when depth returns to 0.
        long arr_start = km_start + 19; // first char after '['
        int d2 = 1;
        long arr_end = arr_start;
        while (arr_end < (long)len) {
            if (buf[arr_end] == '[') d2++;
            else if (buf[arr_end] == ']') {
                d2--;
                if (d2 == 0) break;
            }
            arr_end++;
        }
        // Build quoted needle
        long mer_len = mer_end - mer_start;
        if (mer_len < 256) {
            char needle[260];
            needle[0] = '"';
            memcpy(needle + 1, buf + mer_start, (size_t)mer_len);
            needle[mer_len + 1] = '"';
            needle[mer_len + 2] = '\0';
            if (find_from(buf, arr_end, arr_start, needle, mer_len + 2) >= 0) {
                out[11] = 0.0;
            } else {
                out[11] = 1.0;
            }
        } else {
            out[11] = 1.0;
        }
    } else {
        out[11] = 1.0;
    }

    // Final rounding
    for (int k = 0; k < VEC_DIM; k++) {
        out[k] = round4(out[k]);
    }
    return true;
}
