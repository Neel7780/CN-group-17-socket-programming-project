#ifndef FAULT_H
#define FAULT_H

#include "common.h"

typedef struct {
    bool will_fail;
    uint64_t cut_bytes;
    uint64_t bytes_written;
    double p;
} fault_plan_t;

void fault_plan_init(fault_plan_t *fp, double p, uint64_t rem_payload, uint32_t n_conns, unsigned short seed_state[3]);
ssize_t fault_send(int fd, fault_plan_t *fp, const void *buf, size_t len);
ssize_t fault_sendfile(int out_fd, int in_fd, off_t *offset, size_t count, fault_plan_t *fp);
bool fault_is_cut(const fault_plan_t *fp);
void fault_abort(int fd);

#endif /* FAULT_H */
