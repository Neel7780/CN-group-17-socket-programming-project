#ifndef PROTO_H
#define PROTO_H

#include "common.h"

/*
 * Part II Case 1 wire protocol (Design Document Section 3.2).
 *
 * Case 1 keeps the Part I message syntax byte for byte: no message carries
 * any state, so the only difference from Part I is the behaviour of the two
 * endpoints when the connection dies (Section 3.3).
 *
 *   C: GET <topic>
 *   S: OK <file_count> <total_bytes>   | ERR <code> <REASON>
 *   S: FILE <size> <name>  + <size> raw bytes      (file_count times)
 *   S: END <file_count> <total_bytes>
 *
 * Numbers are unsigned decimal with no sign or padding. The file name is the
 * last token and runs to the end of the line, so it may contain spaces.
 */

typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GET
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char topic[MAX_TOPIC_LEN + 1];
} proto_request_t;

/* True if key is a usable topic: 1..63 chars, no '/', no "..", no spaces or control chars. */
bool proto_valid_topic(const char *key);

/* True if name is safe to create inside the topic directory (no path components). */
bool proto_safe_filename(const char *name);

/* Server side: parse "GET <topic>". Returns 0 on success, -1 on a malformed request. */
int proto_parse_request(const char *line, proto_request_t *req);

/* Server side: format responses into buf. Return the line length, or -1 if it does not fit. */
int proto_fmt_ok(char *buf, size_t cap, uint32_t file_count, uint64_t total_bytes);
int proto_fmt_file_hdr(char *buf, size_t cap, uint64_t size, const char *name);
int proto_fmt_end(char *buf, size_t cap, uint32_t file_count, uint64_t total_bytes);
int proto_fmt_err(char *buf, size_t cap, int code, const char *reason);

/* Client side: parse server lines. Return 0 on success, -1 if the line is not that message. */
int proto_parse_ok(const char *line, uint32_t *file_count, uint64_t *total_bytes);
int proto_parse_file_hdr(const char *line, uint64_t *size, char *name, size_t name_cap);
int proto_parse_end(const char *line, uint32_t *file_count, uint64_t *total_bytes);
int proto_parse_err(const char *line, int *code, const char **reason);

/* Protocol error codes (Design Document Section 3.2). */
#define PROTO_ERR_BAD_REQUEST 400
#define PROTO_ERR_NOT_FOUND   404

#endif /* PROTO_H */
