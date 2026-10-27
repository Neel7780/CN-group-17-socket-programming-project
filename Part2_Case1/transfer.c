#define _GNU_SOURCE
#include "transfer.h"
#include "proto.h"

/*
 * Streams the payload of a topic and finishes with the "END <count> <total>"
 * delimiter.
 *
 * The payload is copied in IO_CHUNK (64 KiB) units. That unit is also the
 * granularity of the per-chunk fault mode, so both fault modes see the same
 * chunking of the byte stream.
 */
int transfer_send_topic(int fd, const topic_t *topic, fault_plan_t *fp, bool use_sendfile,
                        transfer_stats_t *stats) {
    transfer_stats_t local;
    memset(&local, 0, sizeof(local));
    if (!topic) return -1;

    char chunk_buf[IO_CHUNK];
    int rc = -1;

    for (uint32_t idx = 0; idx < topic->file_count; idx++) {
        const file_entry_t *fe = &topic->files[idx];
        uint64_t fsize = fe->size;

        /* 1. Send the FILE header: size and name. */
        char hdr[MAX_LINE];
        int hlen = proto_fmt_file_hdr(hdr, sizeof(hdr), fsize, fe->name);
        if (hlen <= 0) goto done;

        ssize_t hsent = fault_send(fp, fd, hdr, (size_t)hlen);
        if (hsent != hlen) {
            log_msg(LOG_INFO, "Connection cut while sending the FILE header of %s", fe->name);
            goto done;
        }
        log_msg(LOG_DEBUG, "TX FILE idx=%u size=%" PRIu64 " name=%s", idx, fsize, fe->name);

        if (fsize == 0) {                 /* Empty file: the header is the whole file */
            local.files_done++;
            continue;
        }

        /* 2. Send the payload. */
        char fpath[PATH_MAX * 2];
        if ((size_t)snprintf(fpath, sizeof(fpath), "%s/%s", topic->path, fe->name) >= sizeof(fpath)) {
            log_msg(LOG_ERR, "Topic path too long for file %s", fe->name);
            goto done;
        }

        int file_fd = open(fpath, O_RDONLY);
        if (file_fd < 0) {
            log_msg(LOG_ERR, "Failed to open file %s: %s", fpath, strerror(errno));
            goto done;
        }

        uint64_t sent = 0;
        bool cut = false;

        while (sent < fsize) {
            uint64_t left = fsize - sent;
            size_t want = (left < (uint64_t)IO_CHUNK) ? (size_t)left : (size_t)IO_CHUNK;

            /*
             * Per-chunk fault mode: the coin is flipped before every chunk.
             * In request mode this call does nothing.
             */
            if (fault_chunk_cut(fp, fd)) {
                cut = true;
                break;
            }

            if (use_sendfile) {
                off_t off = (off_t)sent;
                ssize_t n = fault_sendfile(fp, fd, file_fd, &off, want);
                if (n < 0) {
                    close(file_fd);
                    goto done;
                }
                sent += (uint64_t)n;
                local.payload_bytes += (uint64_t)n;
                if ((size_t)n != want) {
                    cut = true;   /* the connection was cut mid-chunk */
                    break;
                }
            } else {
                ssize_t r = read(file_fd, chunk_buf, want);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    close(file_fd);
                    goto done;
                }
                if (r == 0) break;   /* the file shrank under us */

                ssize_t w = fault_send(fp, fd, chunk_buf, (size_t)r);
                if (w < 0) {
                    close(file_fd);
                    goto done;
                }
                sent += (uint64_t)w;
                local.payload_bytes += (uint64_t)w;
                if ((size_t)w != (size_t)r) {
                    cut = true;   /* the connection was cut mid-chunk */
                    break;
                }
            }
        }
        close(file_fd);

        if (cut || sent != fsize) {
            log_msg(LOG_INFO, "Connection cut after %" PRIu64 " of %" PRIu64 " bytes of file %u",
                    sent, fsize, idx);
            goto done;
        }
        local.files_done++;
    }

    /* 3. Terminate the topic stream. */
    {
        char end[MAX_LINE];
        int elen = proto_fmt_end(end, sizeof(end), topic->file_count, topic->total_bytes);
        if (elen <= 0) goto done;

        ssize_t esent = fault_send(fp, fd, end, (size_t)elen);
        if (esent != elen) goto done;
    }

    rc = 0;

done:
    if (stats) *stats = local;
    return rc;
}
