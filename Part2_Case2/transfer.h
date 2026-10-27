#ifndef TRANSFER_H
#define TRANSFER_H

#include "common.h"
#include "topics.h"
#include "fault.h"
#include "session.h"
#include "proto.h"

/*
 * Streams the payload of a topic to the client starting at (first_idx,
 * offset) and finishes with the "END <count> <total>" delimiter.
 *
 * The session high-water mark is advanced after every completed file, so
 * that a subsequent RESUME can be validated against it.
 */
int transfer_send_topic(int fd, const topic_t *topic, uint32_t first_idx, uint64_t offset,
                        fault_plan_t *fp, session_t *s, bool use_sendfile);

#endif /* TRANSFER_H */
