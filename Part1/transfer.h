#ifndef TRANSFER_H
#define TRANSFER_H

#include "common.h"
#include "topics.h"

/* Bytes actually put on the wire for one server-side request. */
typedef struct {
    uint64_t payload_bytes;  /* File data bytes sent */
    uint32_t files_done;     /* Files whose payload was sent completely */
} transfer_stats_t;

/*
 * Streams an entire topic to fd and writes the closing "END <count> <total>"
 * delimiter. Returns 0 on success, -1 if the transfer was cut short by any
 * error (the caller is responsible for closing fd).
 */
int transfer_send_topic(int fd, const topic_t *topic, transfer_stats_t *stats);

#endif /* TRANSFER_H */
