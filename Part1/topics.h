#ifndef TOPICS_H
#define TOPICS_H

#include "common.h"

/*
 * Immutable topic metadata, built once at server startup and read
 * concurrently by every worker thread without locking.
 *
 * The directory is scanned with opendir()/readdir(); every regular file
 * becomes a file_entry_t. Entries are sorted alphabetically so the server
 * always streams files in a deterministic order regardless of filesystem
 * readdir() ordering.
 */
typedef struct {
    char name[256];     /* File name in the topic folder */
    uint64_t size;      /* Size in bytes from stat() */
} file_entry_t;

typedef struct {
    char key[MAX_TOPIC_LEN + 1];  /* Topic identifier (e.g. "cane") */
    char path[PATH_MAX];          /* Absolute path from realpath() */
    file_entry_t *files;          /* Array sorted alphabetically */
    uint32_t file_count;          /* Number of regular files */
    uint64_t total_bytes;         /* Total payload bytes */
} topic_t;

typedef struct {
    topic_t *topics;   /* Sorted array; read-only after init */
    uint32_t count;    /* Total active topics */
} topic_map_t;

/* Scans root_dir once and builds the immutable topic map. Returns 0 on success, -1 on error. */
int topic_map_build(topic_map_t *map, const char *root_dir);

/*
 * Looks up a topic by key (case-insensitive). Returns a pointer into the
 * map's internal array (never free it), or NULL if the topic does not exist.
 */
const topic_t *topic_lookup(const topic_map_t *map, const char *key);

/* Frees all memory owned by the map. */
void topic_map_free(topic_map_t *map);

#endif /* TOPICS_H */
