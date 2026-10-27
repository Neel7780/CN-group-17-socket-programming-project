#define _GNU_SOURCE
#include "proto.h"

/*
 * Parses an unsigned decimal at *p and advances *p past it. Unlike strtoul()
 * and sscanf("%lu"), this rejects signs, leading spaces, and overflow.
 */
static int parse_u64(const char **p, uint64_t *out) {
    const char *s = *p;
    if (*s < '0' || *s > '9') return -1;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;
        v = v * 10 + d;
        s++;
    }
    *out = v;
    *p = s;
    return 0;
}

static int parse_u32(const char **p, uint32_t *out) {
    uint64_t v;
    if (parse_u64(p, &v) != 0 || v > UINT32_MAX) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Matches "<verb> " at the start of line and returns the text after it. */
static const char *skip_verb(const char *line, const char *verb) {
    size_t n = strlen(verb);
    if (strncmp(line, verb, n) != 0 || line[n] != ' ') return NULL;
    return line + n + 1;
}

/* Parses "<verb> <u32> <u64>" with nothing after it (shared by OK and END). */
static int parse_count_total(const char *line, const char *verb,
                             uint32_t *file_count, uint64_t *total_bytes) {
    const char *p = skip_verb(line, verb);
    uint32_t count;
    uint64_t total;
    if (!p || parse_u32(&p, &count) != 0 || *p++ != ' ' ||
        parse_u64(&p, &total) != 0 || *p != '\0') {
        return -1;
    }
    *file_count = count;
    *total_bytes = total;
    return 0;
}

bool proto_valid_topic(const char *key) {
    if (!key) return false;
    size_t len = strlen(key);
    if (len == 0 || len > MAX_TOPIC_LEN) return false;
    if (strchr(key, '/') || strstr(key, "..")) return false;
    for (const unsigned char *c = (const unsigned char *)key; *c; c++) {
        if (*c <= ' ' || *c == 0x7f) return false;
    }
    return true;
}

bool proto_safe_filename(const char *name) {
    if (!name || name[0] == '\0') return false;
    if (strlen(name) > MAX_NAME_LEN) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strchr(name, '/')) return false;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if (*c < ' ' || *c == 0x7f) return false;
    }
    return true;
}

int proto_parse_request(const char *line, proto_request_t *req) {
    if (!line || !req) return -1;
    memset(req, 0, sizeof(*req));

    const char *topic = skip_verb(line, "GET");
    if (!topic || !proto_valid_topic(topic)) {
        req->type = CMD_UNKNOWN;
        return -1;
    }
    req->type = CMD_GET;
    memcpy(req->topic, topic, strlen(topic) + 1);
    return 0;
}

static int fmt_result(int len, size_t cap) {
    return (len < 0 || (size_t)len >= cap) ? -1 : len;
}

int proto_fmt_ok(char *buf, size_t cap, uint32_t file_count, uint64_t total_bytes) {
    return fmt_result(snprintf(buf, cap, "OK %" PRIu32 " %" PRIu64 "\n", file_count, total_bytes), cap);
}

int proto_fmt_file_hdr(char *buf, size_t cap, uint64_t size, const char *name) {
    return fmt_result(snprintf(buf, cap, "FILE %" PRIu64 " %s\n", size, name), cap);
}

int proto_fmt_end(char *buf, size_t cap, uint32_t file_count, uint64_t total_bytes) {
    return fmt_result(snprintf(buf, cap, "END %" PRIu32 " %" PRIu64 "\n", file_count, total_bytes), cap);
}

int proto_fmt_err(char *buf, size_t cap, int code, const char *reason) {
    return fmt_result(snprintf(buf, cap, "ERR %d %s\n", code, reason), cap);
}

int proto_parse_ok(const char *line, uint32_t *file_count, uint64_t *total_bytes) {
    return parse_count_total(line, "OK", file_count, total_bytes);
}

int proto_parse_end(const char *line, uint32_t *file_count, uint64_t *total_bytes) {
    return parse_count_total(line, "END", file_count, total_bytes);
}

int proto_parse_file_hdr(const char *line, uint64_t *size, char *name, size_t name_cap) {
    const char *p = skip_verb(line, "FILE");
    uint64_t sz;
    if (!p || parse_u64(&p, &sz) != 0 || *p++ != ' ') return -1;

    /* The name is everything up to the end of the line */
    size_t len = strlen(p);
    if (len == 0 || len >= name_cap) return -1;
    memcpy(name, p, len + 1);
    *size = sz;
    return 0;
}

int proto_parse_err(const char *line, int *code, const char **reason) {
    const char *p = skip_verb(line, "ERR");
    uint32_t c;
    if (!p || parse_u32(&p, &c) != 0 || c > 999) return -1;
    if (*p == ' ') p++;
    else if (*p != '\0') return -1;
    *code = (int)c;
    if (reason) *reason = p;
    return 0;
}
