#define _GNU_SOURCE
#include "common.h"
#include <stdarg.h>

int g_log_level = LOG_INFO;

void log_msg(int level, const char *fmt, ...) {
    if (level > g_log_level) return;
    va_list args;
    va_start(args, fmt);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);
    fprintf(stderr, "[%s.%03ld] ", time_str, ts.tv_nsec / 1000000);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void lr_init(line_reader_t *lr, int fd) {
    lr->fd = fd;
    lr->pos = 0;
    lr->len = 0;
}

ssize_t lr_readline(line_reader_t *lr, char *out, size_t maxlen) {
    if (maxlen == 0) return -1;
    size_t out_idx = 0;
    while (out_idx + 1 < maxlen) {
        if (lr->pos >= lr->len) {
            ssize_t n = read(lr->fd, lr->buf, sizeof(lr->buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) {
                if (out_idx == 0) return 0; /* EOF */
                break;
            }
            lr->pos = 0;
            lr->len = (size_t)n;
        }

        char c = lr->buf[lr->pos++];
        if (c == '\n') {
            break;
        }
        out[out_idx++] = c;
    }

    /* Strip trailing \r if present */
    if (out_idx > 0 && out[out_idx - 1] == '\r') {
        out_idx--;
    }
    out[out_idx] = '\0';
    return (ssize_t)out_idx;
}

ssize_t lr_read_exact(line_reader_t *lr, void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    size_t total = 0;

    /* First drain any unread bytes in the buffer */
    while (total < n && lr->pos < lr->len) {
        size_t avail = lr->len - lr->pos;
        size_t take = (n - total < avail) ? (n - total) : avail;
        memcpy(p + total, lr->buf + lr->pos, take);
        lr->pos += take;
        total += take;
    }

    /* Read remaining directly from socket */
    while (total < n) {
        ssize_t r = read(lr->fd, p + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            return (ssize_t)total; /* Premature EOF */
        }
        total += (size_t)r;
    }

    return (ssize_t)total;
}

ssize_t send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(fd, p + total, len - total, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (s == 0) break;
        total += (size_t)s;
    }
    return (ssize_t)total;
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int parse_kv_opt(const char *arg, const char *key, long *val_out) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') {
        char *endptr = NULL;
        long v = strtol(arg + klen + 1, &endptr, 10);
        if (endptr && *endptr == '\0') {
            if (val_out) *val_out = v;
            return 1;
        }
    }
    return 0;
}

int ensure_dir_exists(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int set_socket_buffers(int fd, int sndbuf, int rcvbuf) {
    if (sndbuf > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            return -1;
        }
    }
    if (rcvbuf > 0) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            return -1;
        }
    }
    return 0;
}

int set_tcp_cork(int fd, int enable) {
#ifdef TCP_CORK
    return setsockopt(fd, IPPROTO_TCP, TCP_CORK, &enable, sizeof(enable));
#else
    (void)fd; (void)enable;
    return 0;
#endif
}
