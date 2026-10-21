#define _GNU_SOURCE
#include "fault.h"

/*
 * SplitMix64 avalanche function. It is used to decorrelate the experiment
 * seed from the connection id before the two are folded into the erand48()
 * state (see fault_seed_state).
 */
static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

void fault_seed_state(unsigned short out[3], long seed, uint32_t conn_id) {
    uint64_t mixed = splitmix64((uint64_t)seed * 0x100000001B3ULL + (uint64_t)conn_id);
    out[0] = (unsigned short)(mixed & 0xFFFF);
    out[1] = (unsigned short)((mixed >> 16) & 0xFFFF);
    out[2] = (unsigned short)((mixed >> 32) & 0xFFFF);
}

int fault_mode_parse(const char *name, fault_mode_t *mode_out) {
    if (!name || !mode_out) return -1;
    if (strcmp(name, "request") == 0) {
        *mode_out = FAULT_MODE_REQUEST;
        return 0;
    }
    if (strcmp(name, "chunk") == 0) {
        *mode_out = FAULT_MODE_CHUNK;
        return 0;
    }
    return -1;
}

const char *fault_mode_name(fault_mode_t mode) {
    return (mode == FAULT_MODE_CHUNK) ? "chunk" : "request";
}

/*
 * Aborts the connection: SO_LINGER with a zero timeout makes close() send a
 * TCP RST instead of the graceful FIN, which is what a client sees when the
 * server crashes or the link drops.
 */
void fault_abort(int fd) {
    struct linger sl;
    memset(&sl, 0, sizeof(sl));
    sl.l_onoff = 1;
    sl.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(fd);
}

/* Aborts the socket and records that the plan now owns the closed fd. */
static void fault_abort_socket(fault_plan_t *fp, int fd) {
    if (fp->aborted) return;
    fp->aborted = true;
    log_msg(LOG_INFO, "FAULT conn=%u topic=%s cut_at=%" PRIu64 " of=%" PRIu64,
            fp->conn_id, fp->topic ? fp->topic : "-", fp->cut_bytes, fp->total_bytes);
    fault_abort(fd);
}

void fault_plan_init(fault_plan_t *fp, fault_mode_t mode, double p, uint64_t total_bytes,
                     const unsigned short seed_state[3], uint32_t conn_id, const char *topic) {
    if (!fp) return;
    memset(fp, 0, sizeof(*fp));

    fp->mode = mode;
    fp->p = p;
    fp->total_bytes = total_bytes;
    fp->cut_bytes = total_bytes;   /* No cut until proven otherwise */
    fp->conn_id = conn_id;
    fp->topic = topic;
    if (seed_state) {
        fp->seed_state[0] = seed_state[0];
        fp->seed_state[1] = seed_state[1];
        fp->seed_state[2] = seed_state[2];
    }

    if (p <= 0.0) return;
    if (p > 1.0) p = 1.0;
    fp->p = p;

    if (mode == FAULT_MODE_CHUNK) {
        /* The coin is flipped per chunk, so nothing is decided here. */
        return;
    }

    /*
     * Request mode: exactly one Bernoulli trial per request.
     *   u ~ U[0,1) is drawn from the connection's own generator, so a given
     *   (seed, connection id) pair always reproduces the same decision.
     */
    double u = erand48(fp->seed_state);
    if (u >= p) return;              /* No fault for this request */

    fp->will_fail = true;

    /* K ~ U{0, 1, ..., R-1}: the cut always happens before the last byte. */
    if (total_bytes <= 1) {
        fp->cut_bytes = 0;
        return;
    }
    double u_cut = erand48(fp->seed_state);
    uint64_t k = (uint64_t)(u_cut * (double)total_bytes);
    if (k >= total_bytes) k = total_bytes - 1;
    fp->cut_bytes = k;
}

ssize_t fault_send(fault_plan_t *fp, int fd, const void *buf, size_t len) {
    if (!fp) return send_all(fd, buf, len);
    if (fp->aborted) {
        errno = EPIPE;
        return -1;
    }

    if (fp->mode == FAULT_MODE_CHUNK || !fp->will_fail) {
        /* No cut point: write everything, but keep the counter up to date. */
        ssize_t n = send_all(fd, buf, len);
        if (n > 0) fp->bytes_written += (uint64_t)n;
        return n;
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < len) {
        if (fp->bytes_written >= fp->cut_bytes) {
            fault_abort_socket(fp, fd);
            return (ssize_t)total;
        }

        /* Never write past the cut point. */
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
            fault_abort_socket(fp, fd);
            return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}

ssize_t fault_sendfile(fault_plan_t *fp, int fd, int in_fd, off_t *offset, size_t count) {
    if (!fp) {
        size_t total = 0;
        while (total < count) {
            ssize_t s = sock_sendfile_chunk(fd, in_fd, offset, count - total);
            if (s < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (s == 0) break;
            total += (size_t)s;
        }
        return (ssize_t)total;
    }
    if (fp->aborted) {
        errno = EPIPE;
        return -1;
    }

    if (fp->mode == FAULT_MODE_CHUNK || !fp->will_fail) {
        size_t total = 0;
        while (total < count) {
            ssize_t s = sock_sendfile_chunk(fd, in_fd, offset, count - total);
            if (s < 0) {
                if (errno == EINTR) continue;
                return (total > 0) ? (ssize_t)total : -1;
            }
            if (s == 0) break;
            total += (size_t)s;
        }
        fp->bytes_written += (uint64_t)total;
        return (ssize_t)total;
    }

    size_t total = 0;
    while (total < count) {
        if (fp->bytes_written >= fp->cut_bytes) {
            fault_abort_socket(fp, fd);
            return (ssize_t)total;
        }

        uint64_t budget = fp->cut_bytes - fp->bytes_written;
        size_t chunk = ((uint64_t)(count - total) < budget) ? (count - total) : (size_t)budget;
        ssize_t s = sock_sendfile_chunk(fd, in_fd, offset, chunk);
        if (s < 0) {
            if (errno == EINTR) continue;
            return (total > 0) ? (ssize_t)total : -1;
        }
        if (s == 0) break;
        total += (size_t)s;
        fp->bytes_written += (uint64_t)s;

        if (fp->bytes_written >= fp->cut_bytes) {
            fault_abort_socket(fp, fd);
            return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}

bool fault_chunk_cut(fault_plan_t *fp, int fd) {
    if (!fp || fp->mode != FAULT_MODE_CHUNK || fp->p <= 0.0) return false;
    if (fp->aborted) return true;

    /* One coin per chunk; the cut happens at the current byte position. */
    double u = erand48(fp->seed_state);
    if (u >= fp->p) return false;

    fp->will_fail = true;
    fp->cut_bytes = fp->bytes_written;
    fault_abort_socket(fp, fd);
    return true;
}
