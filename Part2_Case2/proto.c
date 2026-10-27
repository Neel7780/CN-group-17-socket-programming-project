#define _GNU_SOURCE
#include "proto.h"

int proto_parse_request(const char *line, proto_request_t *req) {
    if (!line || !req) return -1;
    memset(req, 0, sizeof(*req));

    char verb[32];
    int consumed = 0;
    if (sscanf(line, "%31s%n", verb, &consumed) != 1) {
        return -1;
    }

    const char *rest = line + consumed;
    while (*rest == ' ') rest++;

    if (strcmp(verb, "GET") == 0) {
        req->type = CMD_GET;
        if (sscanf(rest, "%63s", req->topic) != 1) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "RESUME") == 0) {
        req->type = CMD_RESUME;
        if (sscanf(rest, "%32s %63s %u %" SCNu64, req->sid, req->topic,
                   &req->file_idx, &req->offset) != 4) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "DONE") == 0) {
        req->type = CMD_DONE;
        if (sscanf(rest, "%32s", req->sid) != 1) {
            return -1;
        }
        return 0;
    }

    req->type = CMD_UNKNOWN;
    return -1;
}

int proto_send_session(int fd, const char *sid, uint32_t file_count, uint64_t total_bytes) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "SESSION %s %u %" PRIu64 "\n", sid, file_count, total_bytes);
    return send_all(fd, buf, (size_t)len) == len ? 0 : -1;
}

int proto_send_resumed(int fd, uint32_t file_idx, uint64_t offset, uint64_t remaining_bytes) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "RESUMED %u %" PRIu64 " %" PRIu64 "\n",
                       file_idx, offset, remaining_bytes);
    return send_all(fd, buf, (size_t)len) == len ? 0 : -1;
}

int proto_send_file_hdr(int fd, uint32_t idx, uint64_t size, uint64_t start, const char *name) {
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "FILE %u %" PRIu64 " %" PRIu64 " %s\n",
                       idx, size, start, name);
    return send_all(fd, buf, (size_t)len) == len ? 0 : -1;
}

int proto_send_end(int fd, uint32_t file_count, uint64_t total_bytes) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "END %u %" PRIu64 "\n", file_count, total_bytes);
    return send_all(fd, buf, (size_t)len) == len ? 0 : -1;
}

int proto_send_bye(int fd) {
    const char *msg = "BYE\n";
    return send_all(fd, msg, 4) == 4 ? 0 : -1;
}

int proto_send_err(int fd, int code, const char *reason) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "ERR %d %s\n", code, reason);
    return send_all(fd, buf, (size_t)len) == len ? 0 : -1;
}
