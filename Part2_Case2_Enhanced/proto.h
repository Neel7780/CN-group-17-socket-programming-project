#ifndef PROTO_H
#define PROTO_H

#include "common.h"

typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GET,
    CMD_RESUME,
    CMD_ATTACH,
    CMD_FETCH,
    CMD_DONE
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char topic[64];
    char sid[33];
    uint32_t n_conns;
    uint32_t file_idx;
    uint32_t first_idx;
    uint32_t last_idx;
    uint64_t offset;
} proto_request_t;

int proto_parse_request(const char *line, proto_request_t *req);

/* Response senders */
int proto_send_session(int fd, const char *sid, uint32_t file_count, uint64_t total_bytes);
int proto_send_attached(int fd, const char *sid);
int proto_send_manifest_entry(int fd, uint32_t idx, uint64_t size, const char *name);
int proto_send_manifest_end(int fd);
int proto_send_file_hdr(int fd, uint32_t idx, uint64_t size, uint64_t start, const char *name);
int proto_send_batch_end(int fd, uint32_t first_idx, uint32_t last_idx);
int proto_send_bye(int fd);
int proto_send_err(int fd, int code, const char *reason);

#endif /* PROTO_H */
