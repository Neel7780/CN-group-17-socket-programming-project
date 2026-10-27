#ifndef TOPICS_H
#define TOPICS_H

#include "common.h"

/*
 * Immutable topic metadata, built once at server startup and read
 * concurrently by every worker thread without locking
 * (Design Document Section 2, "Lock-Free Concurrent Reads").
 */
typedef struct {
    char name[256];            /* File name in the topic folder */
    uint64_t size;             /* Size in bytes from stat() */
    uint64_t pos;              /* Cumulative payload offset in the topic */
    uint64_t hdr_bytes;        /* Length of this file's "FILE <size> <name>" header */
    uint64_t hdr_pos;          /* Cumulative header bytes before this file */
} file_entry_t;

typedef struct {
    char key[MAX_TOPIC_LEN + 1];  /* Topic identifier (e.g. "cane") */
    char path[PATH_MAX];          /* Absolute path from realpath() */
    file_entry_t *files;          /* Array sorted alphabetically */
    uint32_t file_count;          /* Number of regular files */
    uint64_t total_bytes;         /* Total payload bytes */
    uint64_t total_hdr_bytes;     /* Total length of all file headers */
} topic_t;

typedef struct {
    topic_t *topics;           /* Sorted array; read-only after init */
    uint32_t count;            /* Total active topics */
} topic_map_t;

/* Scans root_dir once and builds the immutable topic map. */
int topic_map_build(topic_map_t *map, const char *root_dir);

/* Case-insensitive topic lookup; NULL if the topic does not exist. */
const topic_t *topic_lookup(const topic_map_t *map, const char *key);

/*
 * Exact size R of the response the server is about to send for a topic:
 * the OK line, every FILE header, all payload bytes and the END line.
 * Computed in O(1) from the precomputed offsets, which is what the fault
 * planner needs to draw a cut point (Design Document Section 4.2, step 1).
 */
uint64_t topic_response_size(const topic_t *topic);

void topic_map_free(topic_map_t *map);

#endif /* TOPICS_H */
