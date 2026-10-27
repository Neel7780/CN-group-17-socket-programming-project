#ifndef FAULT_H
#define FAULT_H

#include "common.h"

/*
 * Per-request fault plan.
 *
 * Design Document Section 4.1/4.2: for each request the server performs
 * exactly one Bernoulli trial with probability p. If the trial fires, a
 * cut-point K is drawn uniformly over the total response bytes R
 * (headers plus payload) and the socket is aborted with a TCP RST once K
 * bytes have been written.
 */
typedef struct {
    bool will_fail;
    uint64_t cut_bytes;      /* Absolute byte offset of the cut point */
    uint64_t bytes_written;  /* Bytes written so far on this connection */
    uint64_t total_bytes;    /* R, the scheduled response size */
    double p;
} fault_plan_t;

/*
 * Plans a fault for one request.
 *
 * rem_payload  payload bytes still to be delivered for this request
 * rem_hdr     header bytes still to be delivered for this request
 * seed_state  per-thread erand48() state
 */
void fault_plan_init(fault_plan_t *fp, double p, uint64_t rem_payload, uint64_t rem_hdr,
                     unsigned short seed_state[3]);

/* Returns true once the cut point has been reached. */
bool fault_is_cut(const fault_plan_t *fp);

/*
 * Writes len bytes, aborting the socket if the cut point is reached first.
 *
 * Returns the number of bytes actually written. When the connection is cut
 * the return value is the partial count (not -1), so that the caller can
 * advance the session high-water mark by the number of bytes that really
 * reached the kernel. A negative return means a genuine write error.
 */
ssize_t fault_send(int fd, fault_plan_t *fp, const void *buf, size_t len);

/* sendfile() variant with the same cut-point semantics. */
ssize_t fault_sendfile(int out_fd, int in_fd, off_t *offset, size_t count, fault_plan_t *fp);

/* Aborts the socket with an ungraceful TCP RST (SO_LINGER 0 + close). */
void fault_abort(int fd);

#endif /* FAULT_H */
