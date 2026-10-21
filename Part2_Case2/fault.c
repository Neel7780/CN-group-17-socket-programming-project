#define _GNU_SOURCE
#include "fault.h"

/*
 * Design Document Section 4.2, step 2: the random variate is drawn with
 * erand48() from a per-thread state seeded with seed ^ connection id, so a
 * given (seed, connection id) pair always reproduces the same fault.
 */
void fault_plan_init(fault_plan_t *fp, double p, uint64_t rem_payload, uint64_t rem_hdr,
                     unsigned short seed_state[3]) {
    if (!fp) return;
    memset(fp, 0, sizeof(*fp));
    fp->p = p;

    if (p <= 0.0) return;
    if (p > 1.0) p = 1.0;

    /* R = total bytes scheduled for transmission on this request. */
    uint64_t total = rem_payload + rem_hdr;
    fp->total_bytes = total;

    double u = erand48(seed_state);
    if (u < p) {
        fp->will_fail = true;
        if (total <= 1) {
            fp->cut_bytes = 0;
        } else {
            /* K ~ U{0, 1, ..., R-1} */
            double u_cut = erand48(seed_state);
            fp->cut_bytes = (uint64_t)(u_cut * (double)total);
            if (fp->cut_bytes >= total) fp->cut_bytes = total - 1;
        }
        log_msg(LOG_INFO, "FAULT conn planned: p=%.2f, R=%" PRIu64 ", cut_at=%" PRIu64,
                p, total, fp->cut_bytes);
    }
}

bool fault_is_cut(const fault_plan_t *fp) {
    return fp && fp->will_fail && fp->bytes_written >= fp->cut_bytes;
}

void fault_abort(int fd) {
    struct linger sl;
    memset(&sl, 0, sizeof(sl));
    sl.l_onoff = 1;
    sl.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(fd);
}

ssize_t fault_send(int fd, fault_plan_t *fp, const void *buf, size_t len) {
    if (!fp || !fp->will_fail) {
        return send_all(fd, buf, len);
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "FAULT executed: cut at %" PRIu64 " bytes", fp->cut_bytes);
            fault_abort(fd);
            /* Report the partial count so the caller can advance the
             * high-water mark by what actually reached the kernel. */
            return (ssize_t)total;
        }

        uint64_t budget = fp->cut_bytes - fp->bytes_written;
        size_t chunk = ((uint64_t)(len - total) < budget) ? (len - total) : (size_t)budget;
        ssize_t s = send(fd, p + total, chunk, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return (total > 0) ? (ssize_t)total : -1;
        }
        if (s == 0) break;

        total += (size_t)s;
        fp->bytes_written += (uint64_t)s;

        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "FAULT executed: cut at %" PRIu64 " bytes", fp->cut_bytes);
            fault_abort(fd);
            return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}

ssize_t fault_sendfile(int out_fd, int in_fd, off_t *offset, size_t count, fault_plan_t *fp) {
    if (!fp || !fp->will_fail) {
        size_t total = 0;
        while (total < count) {
            ssize_t s = sock_sendfile_chunk(out_fd, in_fd, offset, count - total);
            if (s < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (s == 0) break;
            total += (size_t)s;
        }
        return (ssize_t)total;
    }

    size_t total = 0;
    while (total < count) {
        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "FAULT executed in sendfile: cut at %" PRIu64 " bytes", fp->cut_bytes);
            fault_abort(out_fd);
            return (ssize_t)total;
        }

        uint64_t budget = fp->cut_bytes - fp->bytes_written;
        size_t chunk = ((uint64_t)(count - total) < budget) ? (count - total) : (size_t)budget;
        ssize_t s = sock_sendfile_chunk(out_fd, in_fd, offset, chunk);
        if (s < 0) {
            if (errno == EINTR) continue;
            return (total > 0) ? (ssize_t)total : -1;
        }
        if (s == 0) break;

        total += (size_t)s;
        fp->bytes_written += (uint64_t)s;

        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "FAULT executed in sendfile: cut at %" PRIu64 " bytes", fp->cut_bytes);
            fault_abort(out_fd);
            return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}
