#define _GNU_SOURCE
#include "transfer.h"

/*
 * Streams the payload of a topic starting at (first_idx, offset) and
 * finishes with the "END <count> <total>" delimiter.
 *
 * The session high-water mark is advanced after every completed file and
 * after every partial write caused by a fault, so that a subsequent RESUME
 * can be validated against it.
 */
int transfer_send_topic(int fd, const topic_t *topic, uint32_t first_idx, uint64_t offset,
                        fault_plan_t *fp, session_t *s, bool use_sendfile) {
    if (!topic) return -1;
    if (first_idx > topic->file_count) return -1;

    char chunk_buf[65536];

    for (uint32_t idx = first_idx; idx < topic->file_count; idx++) {
        uint64_t start = (idx == first_idx) ? offset : 0;
        uint64_t fsize = topic->files[idx].size;
        const char *fname = topic->files[idx].name;

        /* 1. Send the FILE header: index, total size, start offset, name. */
        char hdr[1024];
        int hlen = snprintf(hdr, sizeof(hdr), "FILE %u %" PRIu64 " %" PRIu64 " %s\n",
                            idx, fsize, start, fname);
        if (hlen <= 0) return -1;

        ssize_t hsent = fault_send(fd, fp, hdr, (size_t)hlen);
        log_msg(LOG_DEBUG, "TX FILE idx=%u size=%" PRIu64 " start=%" PRIu64 " name=%s",
                idx, fsize, start, fname);
        if (hsent != hlen) {
            log_msg(LOG_INFO, "Connection cut while sending FILE header for %s", fname);
            return -1;
        }

        if (start >= fsize) {
            /* Nothing left in this file; it was already fully transferred. */
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

        uint64_t written = 0;
        bool cut = false;

        if (use_sendfile) {
            off_t send_off = (off_t)start;
            ssize_t sent = fault_sendfile(fd, file_fd, &send_off, (size_t)to_transfer, fp);
            close(file_fd);
            if (sent < 0) {
                return -1;
            }
            written = (uint64_t)sent;
            cut = (written != to_transfer);
        } else {
            if (lseek(file_fd, (off_t)start, SEEK_SET) == (off_t)-1) {
                close(file_fd);
                return -1;
            }
            while (written < to_transfer) {
                size_t req = (size_t)(to_transfer - written);
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
                written += (uint64_t)sent;
                if ((uint64_t)sent != (uint64_t)r) {
                    cut = true;   /* the connection was cut mid-chunk */
                    break;
                }
            }
            close(file_fd);
        }

        /*
         * Advance the high-water mark by what actually reached the kernel.
         * A TCP RST discards bytes still buffered in the send queue, so this
         * is a safe upper bound on what the client could have received.
         */
        if (s && written > 0) {
            session_update_hw(s, topic_pos(topic, idx, start + written));
            session_add_bytes_sent(s, written);
        }

        if (cut || written != to_transfer) {
            log_msg(LOG_INFO, "Connection cut after %" PRIu64 " of %" PRIu64 " bytes of file %u",
                    written, to_transfer, idx);
            return -1;
        }

        /* The file is complete: advance the high-water mark past it. */
        if (s) {
            session_update_hw(s, topic_pos(topic, idx, fsize));
        }
    }

    /* 2. Terminate the topic stream. */
    char end[256];
    int elen = snprintf(end, sizeof(end), "END %u %" PRIu64 "\n", topic->file_count, topic->total_bytes);
    if (elen <= 0) return -1;

    ssize_t esent = fault_send(fd, fp, end, (size_t)elen);
    if (esent != elen) {
        return -1;
    }

    return 0;
}
