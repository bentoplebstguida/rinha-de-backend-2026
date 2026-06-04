// server.c - minimal HTTP/1.1 server for Rinha 2026 fraud-score endpoint.
// Single-threaded, epoll-based, zero dependencies. musl libc.
#include "parse.h"

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 8080
#define MAX_EVENTS 1024
#define REQ_BUF_CAP (64 * 1024)
#define MAX_CONN 8192

// Pre-computed response bytes
static const char RESP_OK[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 27\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":1}";
static const size_t RESP_OK_LEN = sizeof(RESP_OK) - 1;

static const char RESP_OK_TRUE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 27\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0}";
static const size_t RESP_OK_TRUE_LEN = sizeof(RESP_OK_TRUE) - 1;

static const char RESP_READY[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 15\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"status\":\"ok\"}";
static const size_t RESP_READY_LEN = sizeof(RESP_READY) - 1;

static const char RESP_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
static const size_t RESP_400_LEN = sizeof(RESP_400) - 1;

static const char RESP_405[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
static const size_t RESP_405_LEN = sizeof(RESP_405) - 1;

static const char RESP_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";
static const size_t RESP_404_LEN = sizeof(RESP_404) - 1;

// Per-connection state
typedef enum {
    CONN_STATE_READING_REQUEST,
    CONN_STATE_READING_BODY,
    CONN_STATE_WRITING_RESPONSE,
    CONN_STATE_CLOSED
} conn_state_t;

typedef struct {
    int fd;
    conn_state_t state;
    // Request buffer
    char *buf;          // heap-allocated per-connection
    size_t cap;         // capacity (== REQ_BUF_CAP)
    size_t len;         // bytes currently in buf
    size_t content_length;
    size_t header_end;  // index of end of headers (start of body)
    int keep_alive;
} conn_t;

static conn_t conns[MAX_CONN];
static int epfd = -1;
static int listen_fd = -1;

// Tree-walk classifier. Returns 0 (legit) or 1 (fraud).
static int classify(const double q[VEC_DIM]) {
    int node = 0;
    // Use a counter to allow the compiler to unroll/optimize branches
    while (1) {
        // Read feature, threshold, left, right, value
        int8_t feat = tree_features[node];
        double thr = tree_thresholds[node];
        int32_t l = tree_left[node];
        int32_t r = tree_right[node];
        if (l < 0) {
            // Terminal
            return tree_values[node];
        }
        // Predicated branch
        if (q[feat] <= thr) {
            node = l;
        } else {
            node = r;
        }
        // Defensive: avoid infinite loop
        if (node < 0 || node >= TREE_NODES) return 0;
    }
}

// Find header end (\r\n\r\n) in buf[0..len)
static long find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (long)i;
        }
    }
    return -1;
}

// Parse Content-Length header value (returns -1 if not found)
static long parse_content_length(const char *headers, size_t hlen) {
    // Case-insensitive search for "Content-Length:"
    const char *needle = "content-length:";
    size_t nlen = 15;
    for (size_t i = 0; i + nlen < hlen; i++) {
        if (strncasecmp(headers + i, needle, nlen) == 0) {
            const char *p = headers + i + nlen;
            while (p < headers + hlen && (*p == ' ' || *p == '\t')) p++;
            long v = 0;
            while (p < headers + hlen && *p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            return v;
        }
    }
    return -1;
}

// Detect "Connection: close" header
static int has_connection_close(const char *headers, size_t hlen) {
    const char *needle = "connection:";
    size_t nlen = 11;
    for (size_t i = 0; i + nlen < hlen; i++) {
        if (strncasecmp(headers + i, needle, nlen) == 0) {
            const char *p = headers + i + nlen;
            while (p < headers + hlen && (*p == ' ' || *p == '\t')) p++;
            if (p + 5 <= headers + hlen &&
                (strncasecmp(p, "close", 5) == 0)) {
                return 1;
            }
        }
    }
    return 0;
}

static int handle_request(conn_t *c) {
    const char *buf = c->buf;
    size_t len = c->len;
    long hend = find_header_end(buf, len);
    if (hend < 0) return -1;  // need more data
    size_t header_len = (size_t)(hend + 4);
    long clen = parse_content_length(buf, header_len);
    if (clen < 0) {
        // No Content-Length, must be 0 or invalid.
        // For fraud-score, we need a body. For /ready, no body.
        clen = 0;
    }
    if (len < header_len + (size_t)clen) return -1;  // need more body
    size_t body_start = header_len;
    size_t body_len = (size_t)clen;

    // Parse method + path
    // Method is in buf[0..space)
    int method_get = (len >= 3 && buf[0] == 'G' && buf[1] == 'E' && buf[2] == 'T');
    int method_post = (len >= 4 && buf[0] == 'P' && buf[1] == 'O' && buf[2] == 'S' && buf[3] == 'T');
    if (!method_get && !method_post) {
        write(c->fd, RESP_405, RESP_405_LEN);
        return has_connection_close(buf, header_len) ? 0 : 1;
    }

    // Find path
    const char *path = buf;
    while (path < buf + len && *path != ' ') path++;
    if (path >= buf + len) return -1;
    path++;
    int path_ready = (path + 6 <= buf + len && memcmp(path, "/ready", 6) == 0 &&
                       (path[6] == ' ' || path[6] == '?'));
    int path_fraud = (path + 12 <= buf + len && memcmp(path, "/fraud-score", 12) == 0 &&
                      (path[12] == ' ' || path[12] == '?'));
    if (!path_ready && !path_fraud) {
        write(c->fd, RESP_404, RESP_404_LEN);
        return has_connection_close(buf, header_len) ? 0 : 1;
    }

    if (path_ready) {
        if (!method_get) {
            write(c->fd, RESP_405, RESP_405_LEN);
            return has_connection_close(buf, header_len) ? 0 : 1;
        }
        write(c->fd, RESP_READY, RESP_READY_LEN);
        return has_connection_close(buf, header_len) ? 0 : 1;
    }

    // /fraud-score
    if (!method_post) {
        write(c->fd, RESP_405, RESP_405_LEN);
        return has_connection_close(buf, header_len) ? 0 : 1;
    }
    if (body_len == 0) {
        write(c->fd, RESP_400, RESP_400_LEN);
        return 0; // close on bad request
    }

    double q[VEC_DIM];
    bool ok = parse_fraud_request(buf + body_start, body_len, q);
    if (!ok) {
        write(c->fd, RESP_400, RESP_400_LEN);
        return 0;
    }
    int cls = classify(q);
    if (cls) {
        write(c->fd, RESP_OK, RESP_OK_LEN);
    } else {
        write(c->fd, RESP_OK_TRUE, RESP_OK_TRUE_LEN);
    }
    return has_connection_close(buf, header_len) ? 0 : 1;
}

// Returns 1 if connection should be closed, 0 if keep-alive
static int on_readable(conn_t *c) {
    char tmp[16384];
    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (c->len + (size_t)n > c->cap) {
                // Header too big or body too big
                write(c->fd, RESP_400, RESP_400_LEN);
                return 1;
            }
            memcpy(c->buf + c->len, tmp, (size_t)n);
            c->len += (size_t)n;
            if (c->state == CONN_STATE_READING_REQUEST) {
                long hend = find_header_end(c->buf, c->len);
                if (hend >= 0) {
                    long clen = parse_content_length(c->buf, (size_t)(hend + 4));
                    if (clen > 0) {
                        c->state = CONN_STATE_READING_BODY;
                        c->content_length = (size_t)clen;
                    } else {
                        c->state = CONN_STATE_WRITING_RESPONSE;
                    }
                }
            }
            if (c->state == CONN_STATE_READING_BODY) {
                size_t header_end = find_header_end(c->buf, c->len) + 4;
                if (c->len >= header_end + c->content_length) {
                    c->state = CONN_STATE_WRITING_RESPONSE;
                }
            }
            if (c->state == CONN_STATE_WRITING_RESPONSE) {
                int close = handle_request(c);
                if (close) {
                    return 1;
                }
                // Reset for keep-alive
                c->len = 0;
                c->content_length = 0;
                c->state = CONN_STATE_READING_REQUEST;
                // Don't read more in this loop iteration - just return to epoll
                return 0;
            }
            // Continue reading
        } else if (n == 0) {
            // EOF
            return 1;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return 1;
        }
    }
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static conn_t *new_conn(int fd) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (conns[i].state == CONN_STATE_CLOSED) {
            conn_t *c = &conns[i];
            c->fd = fd;
            c->state = CONN_STATE_READING_REQUEST;
            c->buf = (char *)malloc(REQ_BUF_CAP);
            if (!c->buf) return NULL;
            c->cap = REQ_BUF_CAP;
            c->len = 0;
            c->content_length = 0;
            c->header_end = 0;
            c->keep_alive = 1;
            return c;
        }
    }
    return NULL;
}

static void close_conn(int idx) {
    conn_t *c = &conns[idx];
    if (c->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    if (c->buf) {
        free(c->buf);
        c->buf = NULL;
    }
    c->state = CONN_STATE_CLOSED;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    // Initialize conns
    for (int i = 0; i < MAX_CONN; i++) {
        conns[i].fd = -1;
        conns[i].state = CONN_STATE_CLOSED;
        conns[i].buf = NULL;
    }

    // Create listen socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 4096) < 0) { perror("listen"); return 1; }
    set_nonblock(listen_fd);

    // Create epoll
    epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                // Accept all
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept4(listen_fd, (struct sockaddr *)&caddr, &clen,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("accept4");
                        break;
                    }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    conn_t *c = new_conn(cfd);
                    if (!c) {
                        close(cfd);
                        continue;
                    }
                    int idx = (int)(c - conns);
                    struct epoll_event cev = {0};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.u64 = (uint64_t)cfd | ((uint64_t)idx << 32);
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                int idx = (int)(events[i].data.u64 >> 32);
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    close_conn(idx);
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    if (on_readable(&conns[idx])) {
                        close_conn(idx);
                    }
                }
            }
        }
    }
    return 0;
}
