#ifndef PROTO_H
#define PROTO_H

#include "common.h"

/*
 * Part II Case 2 wire protocol (Design Document Section 3.4 / Table 3).
 *
 *   C -> S   GET <topic>
 *   S -> C   SESSION <sid> <file count> <total bytes>
 *   C -> S   RESUME <sid> <topic> <file index> <offset>
 *   S -> C   RESUMED <file index> <offset> <remaining bytes>
 *   S -> C   FILE <index> <size> <start> <name>  + (size - start) raw bytes
 *   S -> C   END <file count> <total bytes>
 *   C -> S   DONE <sid>
 *   S -> C   BYE
 *   S -> C   ERR <code> <REASON>              (410 / 409 / 416 / 404 / 400)
 */

typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GET,
    CMD_RESUME,
    CMD_DONE
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char topic[64];
    char sid[33];
    uint32_t file_idx;
    uint64_t offset;
} proto_request_t;

int proto_parse_request(const char *line, proto_request_t *req);

/* Response senders */
int proto_send_session(int fd, const char *sid, uint32_t file_count, uint64_t total_bytes);
int proto_send_resumed(int fd, uint32_t file_idx, uint64_t offset, uint64_t remaining_bytes);
int proto_send_file_hdr(int fd, uint32_t idx, uint64_t size, uint64_t start, const char *name);
int proto_send_end(int fd, uint32_t file_count, uint64_t total_bytes);
int proto_send_bye(int fd);
int proto_send_err(int fd, int code, const char *reason);

/* Error codes used by the resume validation rules (Design Document Section 5.4). */
#define PROTO_ERR_BAD_REQUEST   400
#define PROTO_ERR_NOT_FOUND     404
#define PROTO_ERR_TOPIC_MISMATCH 409
#define PROTO_ERR_SESSION_EXPIRED 410
#define PROTO_ERR_BAD_CHECKPOINT 416

#endif /* PROTO_H */
