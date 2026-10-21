#ifndef TRANSFER_H
#define TRANSFER_H

#include "common.h"
#include "topics.h"
#include "fault.h"

/* What the server actually managed to put on the wire for one request. */
typedef struct {
    uint64_t payload_bytes;  /* File data bytes handed to the kernel */
    uint32_t files_done;     /* Files whose payload was sent completely */
} transfer_stats_t;

/*
 * Streams a whole topic to the client and finishes with the
 * "END <file_count> <total_bytes>" delimiter.
 *
 * Every write goes through the fault plan, so the response can be cut at any
 * byte. The function returns 0 when the whole topic was delivered and -1
 * otherwise; when the connection was cut, fp->aborted is set, which tells the
 * caller that the failure was injected rather than a real I/O error.
 */
int transfer_send_topic(int fd, const topic_t *topic, fault_plan_t *fp, bool use_sendfile,
                        transfer_stats_t *stats);

#endif /* TRANSFER_H */
