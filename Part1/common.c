#define _GNU_SOURCE
#include "common.h"
#include <stdarg.h>

int g_log_level = LOG_INFO;

void log_msg(int level, const char *fmt, ...) {
    if (level > g_log_level) return;

    static const char *const names[] = { "ERR", "WARN", "INFO", "DEBUG" };
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

    /* Format the whole line first so concurrent threads never interleave output */
    char line[MAX_LINE * 2];
    int off = snprintf(line, sizeof(line), "[%s.%03ld] %-5s ", time_str,
                       ts.tv_nsec / 1000000L, names[level < 0 ? 0 : (level > 3 ? 3 : level)]);
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", line);
}

void lr_init(line_reader_t *lr, int fd) {
    lr->fd = fd;
    lr->pos = 0;
    lr->len = 0;
    lr->bytes_in = 0;
}

/* Refills the buffer. Returns bytes read, 0 on EOF, -1 on error. */
static ssize_t lr_fill(line_reader_t *lr) {
    for (;;) {
        ssize_t n = read(lr->fd, lr->buf, sizeof(lr->buf));
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) {
            lr->pos = 0;
            lr->len = (size_t)n;
            lr->bytes_in += (uint64_t)n;
        }
        return n;
    }
}

ssize_t lr_readline(line_reader_t *lr, char *out, size_t maxlen) {
    if (maxlen == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;          /* Bytes of the current line copied into out */
    for (;;) {
        if (lr->pos >= lr->len) {
            ssize_t n = lr_fill(lr);
            if (n < 0) return -1;
            if (n == 0) {
                if (used == 0) return 0;  /* Clean EOF at a line boundary */
                errno = EPROTO;           /* Stream ended mid-line */
                return -1;
            }
        }

        const char *start = lr->buf + lr->pos;
        size_t avail = lr->len - lr->pos;
        const char *nl = memchr(start, '\n', avail);
        size_t take = nl ? (size_t)(nl - start) : avail;

        /* Keep one byte for the NUL terminator */
        if (used + take >= maxlen) {
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(out + used, start, take);
        used += take;

        if (nl) {
            lr->pos += take + 1;
            ssize_t consumed = (ssize_t)(used + 1);
            if (used > 0 && out[used - 1] == '\r') used--;
            out[used] = '\0';
            return consumed;
        }
        lr->pos = lr->len;
    }
}

ssize_t lr_read_exact(line_reader_t *lr, void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    size_t total = 0;

    /* Bytes already pulled in alongside the last header come first */
    if (lr->pos < lr->len) {
        size_t avail = lr->len - lr->pos;
        size_t take = n < avail ? n : avail;
        memcpy(p, lr->buf + lr->pos, take);
        lr->pos += take;
        total = take;
    }

    /* The rest goes straight from the socket into the caller's buffer */
    while (total < n) {
        ssize_t r = read(lr->fd, p + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;  /* Premature EOF */
        total += (size_t)r;
        lr->bytes_in += (uint64_t)r;
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
        total += (size_t)s;
    }
    return (ssize_t)total;
}

ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, p + total, len - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

int ensure_dir_exists(const char *path) {
    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s", path) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t now_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int parse_port(const char *s, int *port_out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535) return -1;
    *port_out = (int)v;
    return 0;
}

int parse_kv_opt(const char *arg, const char *key, long *val_out) {
    const char *val;
    if (!parse_kv_str(arg, key, &val)) return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (errno != 0 || end == val || *end != '\0') return 0;
    if (val_out) *val_out = v;
    return 1;
}

int parse_kv_str(const char *arg, const char *key, const char **val_out) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) != 0 || arg[klen] != '=') return 0;
    if (val_out) *val_out = arg + klen + 1;
    return 1;
}
