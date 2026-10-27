#define _GNU_SOURCE
#include "transfer.h"
#include "proto.h"

/*
 * Streams the files of a topic over fd — the OK line has already been
 * sent by server.c before this is called:
 *   FILE <size> <name>  + <size> raw bytes      (for each file)
 *   END <file_count> <total_bytes>
 *
 * Returns 0 if everything was sent, -1 on the first I/O error.
 */
int transfer_send_topic(int fd, const topic_t *topic, transfer_stats_t *stats) {
    transfer_stats_t local;
    memset(&local, 0, sizeof(local));
    if (!topic) return -1;

    /* 1. Send every file. */
    char chunk[IO_CHUNK];
    int rc = -1;

    for (uint32_t idx = 0; idx < topic->file_count; idx++) {
        const file_entry_t *fe = &topic->files[idx];

        /* FILE header */
        char hdr[MAX_LINE];
        int hlen = proto_fmt_file_hdr(hdr, sizeof(hdr), fe->size, fe->name);
        if (hlen <= 0) goto done;
        if (send_all(fd, hdr, (size_t)hlen) != (ssize_t)hlen) goto done;

        log_msg(LOG_DEBUG, "TX FILE idx=%u size=%" PRIu64 " name=%s", idx, fe->size, fe->name);

        if (fe->size == 0) {
            local.files_done++;
            continue;
        }

        /* Payload */
        char fpath[PATH_MAX * 2];
        if ((size_t)snprintf(fpath, sizeof(fpath), "%s/%s", topic->path, fe->name)
                >= sizeof(fpath)) {
            log_msg(LOG_ERR, "Topic path too long for file %s", fe->name);
            goto done;
        }

        int ffd = open(fpath, O_RDONLY);
        if (ffd < 0) {
            log_msg(LOG_ERR, "Cannot open %s: %s", fpath, strerror(errno));
            goto done;
        }

        uint64_t left = fe->size;
        bool ok_flag = true;
        while (left > 0) {
            size_t want = left < IO_CHUNK ? (size_t)left : IO_CHUNK;
            ssize_t r = read(ffd, chunk, want);
            if (r < 0) {
                if (errno == EINTR) continue;
                log_msg(LOG_ERR, "Read error on %s: %s", fpath, strerror(errno));
                ok_flag = false;
                break;
            }
            if (r == 0) { ok_flag = false; break; }  /* file shrank */

            if (send_all(fd, chunk, (size_t)r) != r) {
                ok_flag = false;
                break;
            }
            left -= (uint64_t)r;
            local.payload_bytes += (uint64_t)r;
        }
        close(ffd);
        if (!ok_flag) goto done;
        local.files_done++;
    }

    /* 2. Send the END delimiter. */
    {
        char end[MAX_LINE];
        int elen = proto_fmt_end(end, sizeof(end), topic->file_count, topic->total_bytes);
        if (elen <= 0) goto done;
        if (send_all(fd, end, (size_t)elen) != (ssize_t)elen) goto done;
    }

    rc = 0;
done:
    if (stats) *stats = local;
    return rc;
}
