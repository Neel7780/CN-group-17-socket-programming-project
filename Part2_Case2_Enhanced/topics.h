#ifndef TOPICS_H
#define TOPICS_H

#include "common.h"

typedef struct {
    char name[256];
    uint64_t size;
    uint64_t pos;      /* Cumulative payload offset in topic */
} file_entry_t;

typedef struct {
    char key[64];              /* Topic identifier (e.g. "cane") */
    char path[PATH_MAX];       /* Absolute path from realpath() */
    file_entry_t *files;       /* Array sorted alphabetically */
    uint32_t file_count;       /* Number of regular files */
    uint64_t total_bytes;      /* Total payload bytes */
} topic_t;

typedef struct {
    topic_t *topics;           /* Sorted array; read-only after init */
    uint32_t count;            /* Total active topics */
} topic_map_t;

int topic_map_build(topic_map_t *map, const char *root_dir);
const topic_t *topic_lookup(const topic_map_t *map, const char *key);
uint64_t topic_pos(const topic_t *topic, uint32_t file_idx, uint64_t offset);
uint64_t topic_stream_size(const topic_t *topic, uint32_t first_idx, uint32_t last_idx, uint64_t offset);
void topic_map_free(topic_map_t *map);

#endif /* TOPICS_H */
