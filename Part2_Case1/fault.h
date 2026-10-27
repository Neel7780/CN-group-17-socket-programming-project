#ifndef FAULT_H
#define FAULT_H

#include "common.h"

/*
 * Fault injection (Design Document Section 4).
 *
 * Two fault modes are supported:
 *
 *   FAULT_MODE_REQUEST  The default and the model used by the experiments.
 *                       For every request the server performs exactly one
 *                       Bernoulli trial with probability p. If the trial
 *                       fires, a cut point K is drawn uniformly over the R
 *                       response bytes (headers plus payload) and the
 *                       connection is aborted with a TCP RST as soon as K
 *                       bytes have been written. Every reconnection is a new
 *                       request with its own independent trial, so p is the
 *                       probability that one request experiences one
 *                       injected failure.
 *
 *   FAULT_MODE_CHUNK    An alternative model in which the coin is flipped
 *                       once per 64 KiB chunk instead of once per request.
 *                       It is kept for the extra experiments of Section 4.3;
 *                       the probability given on the command line is then
 *                       the per-chunk error rate q.
 */
typedef enum {
    FAULT_MODE_REQUEST = 0,
    FAULT_MODE_CHUNK = 1
} fault_mode_t;

/*
 * The fault plan of a single request. It owns the cut point and the byte
 * counter that enforces it.
 */
typedef struct {
    fault_mode_t mode;
    double p;                 /* Failure probability of the mode in use */
    uint64_t total_bytes;     /* R: the scheduled size of the response */
    uint64_t cut_bytes;       /* Absolute byte offset of the cut point */
    uint64_t bytes_written;   /* Bytes handed to the kernel so far */
    bool will_fail;           /* A cut is scheduled for this request */
    bool aborted;             /* The socket has been aborted by this plan */
    uint32_t conn_id;         /* Connection the plan belongs to */
    const char *topic;        /* Topic of the request (for the log line) */
    unsigned short seed_state[3]; /* erand48() state of the connection */
} fault_plan_t;

/* "request" or "chunk". Returns 0 on success, -1 if the name is unknown. */
int fault_mode_parse(const char *name, fault_mode_t *mode_out);
const char *fault_mode_name(fault_mode_t mode);

/*
 * Derives the erand48() state of a connection from the experiment seed and
 * the connection id (Design Document Section 4.2, step 2). The two words are
 * mixed with SplitMix64 first: raw words are correlated, which makes the
 * fault decision of consecutive connections behave systematically instead of
 * following the requested Bernoulli distribution.
 */
void fault_seed_state(unsigned short out[3], long seed, uint32_t conn_id);

/*
 * Plans the fault of one request.
 *
 * total_bytes is R, the exact number of bytes the response will contain.
 * seed_state is the per-connection erand48() state, which the plan advances
 * as it draws its random variates.
 */
void fault_plan_init(fault_plan_t *fp, fault_mode_t mode, double p, uint64_t total_bytes,
                     const unsigned short seed_state[3], uint32_t conn_id, const char *topic);

/*
 * Writes len bytes, never passing the cut point.
 *
 * Returns the number of bytes actually written. When the connection is cut
 * the return value is the partial count (not -1), so that the caller can
 * account for the bytes that really reached the kernel. A negative return
 * means a genuine write error.
 */
ssize_t fault_send(fault_plan_t *fp, int fd, const void *buf, size_t len);

/* sendfile() variant with the same cut-point semantics. */
ssize_t fault_sendfile(fault_plan_t *fp, int fd, int in_fd, off_t *offset, size_t count);

/*
 * Chunk mode only: draws the coin for the next chunk. If the coin comes up
 * tails the socket is aborted and true is returned; the caller must then stop
 * writing. In request mode this is a no-op that returns false.
 */
bool fault_chunk_cut(fault_plan_t *fp, int fd);

/* Aborts the socket with an ungraceful TCP RST (SO_LINGER 0 + close). */
void fault_abort(int fd);

#endif /* FAULT_H */
