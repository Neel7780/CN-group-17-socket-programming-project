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
        req->n_conns = 1;
        if (sscanf(rest, "%63s %u", req->topic, &req->n_conns) < 1) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "ATTACH") == 0) {
        req->type = CMD_ATTACH;
        if (sscanf(rest, "%32s", req->sid) != 1) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "FETCH") == 0) {
        req->type = CMD_FETCH;
        if (sscanf(rest, "%32s %u %u %lu", req->sid, &req->first_idx, &req->last_idx, &req->offset) != 4) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "DONE") == 0) {
        req->type = CMD_DONE;
        if (sscanf(rest, "%32s", req->sid) != 1) {
            return -1;
        }
        return 0;
    } else if (strcmp(verb, "RESUME") == 0) {
        req->type = CMD_RESUME;
        if (sscanf(rest, "%32s %63s %u %lu", req->sid, req->topic, &req->file_idx, &req->offset) != 4) {
            return -1;
        }
        return 0;
    }

    req->type = CMD_UNKNOWN;
    return -1;
}

int proto_send_session(int fd, const char *sid, uint32_t file_count, uint64_t total_bytes) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "SESSION %s %u %lu\n", sid, file_count, total_bytes);
    return send_all(fd, buf, len) == len ? 0 : -1;
}

int proto_send_attached(int fd, const char *sid) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "ATTACHED %s\n", sid);
    return send_all(fd, buf, len) == len ? 0 : -1;
}

int proto_send_manifest_entry(int fd, uint32_t idx, uint64_t size, const char *name) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "M %u %lu %s\n", idx, size, name);
    return send_all(fd, buf, len) == len ? 0 : -1;
}

int proto_send_manifest_end(int fd) {
    const char *msg = "MEND\n";
    return send_all(fd, msg, 5) == 5 ? 0 : -1;
}

int proto_send_file_hdr(int fd, uint32_t idx, uint64_t size, uint64_t start, const char *name) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "FILE %u %lu %lu %s\n", idx, size, start, name);
    return send_all(fd, buf, len) == len ? 0 : -1;
}

int proto_send_batch_end(int fd, uint32_t first_idx, uint32_t last_idx) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "BEND %u %u\n", first_idx, last_idx);
    return send_all(fd, buf, len) == len ? 0 : -1;
}

int proto_send_bye(int fd) {
    const char *msg = "BYE\n";
    return send_all(fd, msg, 4) == 4 ? 0 : -1;
}

int proto_send_err(int fd, int code, const char *reason) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "ERR %d %s\n", code, reason);
    return send_all(fd, buf, len) == len ? 0 : -1;
}
