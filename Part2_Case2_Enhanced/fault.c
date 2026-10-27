#define _GNU_SOURCE
#include "fault.h"
#include <sys/uio.h>

void fault_plan_init(fault_plan_t *fp, double p, uint64_t rem_payload, uint32_t n_conns, unsigned short seed_state[3]) {
    fp->p = p;
    fp->bytes_written = 0;
    fp->will_fail = false;
    fp->cut_bytes = UINT64_MAX;

    if (p <= 0.0) return;
    if (n_conns == 0) n_conns = 1;

    double u = erand48(seed_state);
    if (u < p) {
        fp->will_fail = true;
        uint64_t share = rem_payload / n_conns;
        if (share <= 1) {
            fp->cut_bytes = 0;
        } else {
            double u_cut = erand48(seed_state);
            fp->cut_bytes = (uint64_t)(u_cut * (double)(share - 1));
        }
        log_msg(LOG_INFO, "Fault planned: p=%.2f, share=%lu, cut_at=%lu", p, share, fp->cut_bytes);
    }
}

void fault_abort(int fd) {
    struct linger sl = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(fd);
}

bool fault_is_cut(const fault_plan_t *fp) {
    return fp && fp->will_fail && fp->bytes_written >= fp->cut_bytes;
}

ssize_t fault_send(int fd, fault_plan_t *fp, const void *buf, size_t len) {
    if (!fp || !fp->will_fail) {
        return send_all(fd, buf, len);
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        if (fp->bytes_written >= fp->cut_bytes) {
            fault_abort(fd);
            return -1;
        }

        uint64_t budget = fp->cut_bytes - fp->bytes_written;
        size_t chunk = (len - total < budget) ? (len - total) : (size_t)budget;
        ssize_t s = send(fd, p + total, chunk, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (s == 0) break;

        total += (size_t)s;
        fp->bytes_written += (uint64_t)s;

        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "Fault executed: cut at %lu bytes", fp->cut_bytes);
            fault_abort(fd);
            return -1;
        }
    }
    return (ssize_t)total;
}

ssize_t fault_sendfile(int out_fd, int in_fd, off_t *offset, size_t count, fault_plan_t *fp) {
    if (!fp || !fp->will_fail) {
        size_t total = 0;
        while (total < count) {
#ifdef __APPLE__
            struct sf_hdtr hdr = {0};
            ssize_t s = sendfile(out_fd, in_fd, *offset, offset, &hdr, 0);
            if (s > 0) {
                // macOS sendfile updates *offset, but we need to track total for the loop
                // The total bytes sent is s.
            }
#else
            ssize_t s = sendfile(out_fd, in_fd, offset, count - total);
#endif
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
            fault_abort(out_fd);
            return -1;
        }

#ifdef __APPLE__
            struct sf_hdtr hdr = {0};
            ssize_t s = sendfile(out_fd, in_fd, *offset, offset, &hdr, 0);
#else
            uint64_t budget = fp->cut_bytes - fp->bytes_written;
            size_t chunk = (count - total < budget) ? (count - total) : (size_t)budget;
            ssize_t s = sendfile(out_fd, in_fd, offset, chunk);
#endif
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (s == 0) break;

        total += (size_t)s;
        fp->bytes_written += (uint64_t)s;

        if (fp->bytes_written >= fp->cut_bytes) {
            log_msg(LOG_INFO, "Fault executed in sendfile: cut at %lu bytes", fp->cut_bytes);
            fault_abort(out_fd);
            return -1;
        }
    }
    return (ssize_t)total;
}
