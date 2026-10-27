#ifndef MANIFEST_H
#define MANIFEST_H

#include "common.h"
#include "topics.h"
#include "workq.h"

typedef struct {
    file_entry_t *files;
    uint32_t count;
    uint64_t total_bytes;
} manifest_t;

int manifest_receive(line_reader_t *lr, manifest_t *m, uint32_t expected_count, uint64_t expected_bytes);
int manifest_partition_batches(const manifest_t *m, workq_t *q, uint64_t target_batch_bytes);
void manifest_free(manifest_t *m);

#endif /* MANIFEST_H */
