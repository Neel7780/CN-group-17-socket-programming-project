#define _GNU_SOURCE
#include "manifest.h"

int manifest_receive(line_reader_t *lr, manifest_t *m, uint32_t expected_count, uint64_t expected_bytes) {
    if (!lr || !m) return -1;
    m->files = calloc(expected_count, sizeof(file_entry_t));
    if (!m->files) return -1;
    m->count = expected_count;
    m->total_bytes = 0;

    char line[MAX_LINE];
    uint32_t received_entries = 0;

    while (1) {
        ssize_t len = lr_readline(lr, line, sizeof(line));
        if (len <= 0) {
            free(m->files);
            m->files = NULL;
            return -1;
        }

        if (strcmp(line, "MEND") == 0) {
            break;
        }

        uint32_t idx = 0;
        uint64_t size = 0;
        char name[256];
        if (sscanf(line, "M %u %llu %255s", &idx, &size, name) != 3) {
            log_msg(LOG_ERR, "Malformed manifest line: %s", line);
            free(m->files);
            m->files = NULL;
            return -1;
        }

        if (idx < expected_count) {
            m->files[idx].size = size;
            snprintf(m->files[idx].name, sizeof(m->files[idx].name), "%s", name);
            m->total_bytes += size;
            received_entries++;
        }
    }

    if (received_entries != expected_count || m->total_bytes != expected_bytes) {
        log_msg(LOG_ERR, "Manifest mismatch: got %u files (%lu bytes), expected %u files (%lu bytes)",
                received_entries, m->total_bytes, expected_count, expected_bytes);
        free(m->files);
        m->files = NULL;
        return -1;
    }

    /* Compute cumulative pos */
    uint64_t cpos = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        m->files[i].pos = cpos;
        cpos += m->files[i].size;
    }

    log_msg(LOG_INFO, "Manifest received successfully: %u files, %lu bytes", m->count, m->total_bytes);
    return 0;
}

int manifest_partition_batches(const manifest_t *m, workq_t *q, uint64_t target_batch_bytes) {
    if (!m || !q || m->count == 0) return 0;
    if (target_batch_bytes == 0) target_batch_bytes = DEFAULT_BATCH_BYTES;

    int batches = 0;
    uint32_t start_idx = 0;
    uint64_t cur_batch_bytes = 0;

    for (uint32_t i = 0; i < m->count; i++) {
        cur_batch_bytes += m->files[i].size;
        if (cur_batch_bytes >= target_batch_bytes || i == m->count - 1) {
            workq_push_back(q, start_idx, i, 0);
            batches++;
            start_idx = i + 1;
            cur_batch_bytes = 0;
        }
    }
    log_msg(LOG_INFO, "Partitioned manifest into %d parallel batches", batches);
    return batches;
}

void manifest_free(manifest_t *m) {
    if (!m) return;
    free(m->files);
    m->files = NULL;
    m->count = 0;
    m->total_bytes = 0;
}
