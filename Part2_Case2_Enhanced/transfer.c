#define _GNU_SOURCE
#include "transfer.h"

int transfer_send_batch(int fd, const topic_t *topic, uint32_t first_idx, uint32_t last_idx,
                        uint64_t offset, fault_plan_t *fp, session_t *s,
                        bool use_sendfile, bool use_cork) {
    if (!topic || first_idx > last_idx || last_idx >= topic->file_count) {
        return -1;
    }

    if (use_cork) {
        set_tcp_cork(fd, 1);
    }

    char chunk_buf[65536];

    for (uint32_t idx = first_idx; idx <= last_idx; idx++) {
        uint64_t start = (idx == first_idx) ? offset : 0;
        uint64_t fsize = topic->files[idx].size;
        const char *fname = topic->files[idx].name;

        /* 1. Send FILE header */
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr), "FILE %u %llu %llu %s\n", idx, (unsigned long long)fsize, (unsigned long long)start, fname);
        if (fault_send(fd, fp, hdr, hlen) != hlen) {
            log_msg(LOG_INFO, "Connection failed while sending FILE header for %s", fname);
            return -1;
        }

        if (start >= fsize) {
            /* Empty or already fully sent file */
            continue;
        }

        uint64_t to_transfer = fsize - start;

        char fpath[PATH_MAX * 2];
        snprintf(fpath, sizeof(fpath), "%s/%s", topic->path, fname);
        int file_fd = open(fpath, O_RDONLY);
        if (file_fd < 0) {
            log_msg(LOG_ERR, "Failed to open file %s: %s", fpath, strerror(errno));
            return -1;
        }

        if (use_sendfile) {
            off_t send_off = (off_t)start;
            ssize_t sent = fault_sendfile(fd, file_fd, &send_off, to_transfer, fp);
            close(file_fd);
            if (sent < 0 || (size_t)sent != to_transfer) {
                if (s && sent > 0) {
                    pthread_mutex_lock(&s->mu);
                    uint64_t p = topic_pos(topic, idx, start + (uint64_t)sent);
                    if (p > s->hw_pos) s->hw_pos = p;
                    s->bytes_sent += (uint64_t)sent;
                    pthread_mutex_unlock(&s->mu);
                }
                return -1;
            }
        } else {
            if (lseek(file_fd, (off_t)start, SEEK_SET) == (off_t)-1) {
                close(file_fd);
                return -1;
            }
            uint64_t transferred = 0;
            while (transferred < to_transfer) {
                size_t req = to_transfer - transferred;
                size_t csize = (req < sizeof(chunk_buf)) ? req : sizeof(chunk_buf);
                ssize_t r = read(file_fd, chunk_buf, csize);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    close(file_fd);
                    return -1;
                }
                if (r == 0) break;

                ssize_t sent = fault_send(fd, fp, chunk_buf, (size_t)r);
                if (sent < 0) {
                    close(file_fd);
                    return -1;
                }
                transferred += (uint64_t)sent;
            }
            close(file_fd);
            if (transferred != to_transfer) {
                return -1;
            }
        }

        /* Update session high-water mark */
        if (s) {
            pthread_mutex_lock(&s->mu);
            uint64_t p = topic_pos(topic, idx, fsize);
            if (p > s->hw_pos) s->hw_pos = p;
            s->bytes_sent += to_transfer;
            pthread_mutex_unlock(&s->mu);
        }
    }

    /* 2. Send BEND terminator */
    char bend[128];
    int blen = snprintf(bend, sizeof(bend), "BEND %u %u\n", first_idx, last_idx);
    if (fault_send(fd, fp, bend, blen) != blen) {
        return -1;
    }

    if (use_cork) {
        set_tcp_cork(fd, 0);
    }

    return 0;
}
