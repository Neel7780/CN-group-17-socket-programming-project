#ifndef TRANSFER_H
#define TRANSFER_H

#include "common.h"
#include "topics.h"
#include "fault.h"
#include "session.h"
#include "proto.h"

int transfer_send_batch(int fd, const topic_t *topic, uint32_t first_idx, uint32_t last_idx,
                        uint64_t offset, fault_plan_t *fp, session_t *s,
                        bool use_sendfile, bool use_cork);

#endif /* TRANSFER_H */
