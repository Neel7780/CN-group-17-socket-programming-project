#define _GNU_SOURCE
#include "topics.h"

static int compare_files(const void *a, const void *b) {
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    return strcmp(fa->name, fb->name);
}

static int compare_topics(const void *a, const void *b) {
    const topic_t *ta = (const topic_t *)a;
    const topic_t *tb = (const topic_t *)b;
    return strcmp(ta->key, tb->key);
}

/*
 * Length of the "FILE <idx> <size> <start> <name>\n" header. The server
 * precomputes this at index time so that the exact response length R of a
 * transfer can be computed in O(1) when planning a fault (Design Document
 * Section 4.2, step 1).
 */
static uint64_t file_hdr_len(uint32_t idx, uint64_t size, uint64_t start, const char *name) {
    /* "FILE " + idx + " " + size + " " + start + " " + name + "\n" */
    uint64_t len = 5;
    len += 1; for (uint32_t v = idx; v >= 10; v /= 10) len++;
    len += 1; for (uint64_t v = size; v >= 10; v /= 10) len++;
    len += 1; for (uint64_t v = start; v >= 10; v /= 10) len++;
    len += strlen(name);
    len += 1; /* '\n' */
    return len;
}

static int scan_topic_directory(topic_t *t, const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    size_t cap = 32;
    t->files = malloc(cap * sizeof(file_entry_t));
    if (!t->files) {
        closedir(d);
        return -1;
    }
    t->file_count = 0;
    t->total_bytes = 0;
    t->total_hdr_bytes = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue; /* Skip hidden files */
        if (strchr(de->d_name, '\n') != NULL) continue; /* Keep headers single-line */

        char fpath[PATH_MAX * 2];
        if ((size_t)snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, de->d_name) >= sizeof(fpath)) continue;

        struct stat st;
        if (stat(fpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue; /* Only regular files */

        if (t->file_count >= cap) {
            cap *= 2;
            file_entry_t *nf = realloc(t->files, cap * sizeof(file_entry_t));
            if (!nf) {
                closedir(d);
                return -1;
            }
            t->files = nf;
        }

        file_entry_t *fe = &t->files[t->file_count++];
        snprintf(fe->name, sizeof(fe->name), "%s", de->d_name);
        fe->size = (uint64_t)st.st_size;
        fe->pos = 0;
        fe->hdr_bytes = 0;
        fe->hdr_pos = 0;
    }
    closedir(d);

    /* Sort files alphabetically */
    if (t->file_count > 0) {
        qsort(t->files, t->file_count, sizeof(file_entry_t), compare_files);
    }

    /*
     * Compute cumulative payload and header offsets. Because the index of
     * every file is now fixed, the header length of every file is fixed too
     * (with start = 0), which lets topic_hdr_size() answer in O(1).
     */
    uint64_t cur_pos = 0;
    uint64_t cur_hdr = 0;
    for (uint32_t i = 0; i < t->file_count; i++) {
        t->files[i].pos = cur_pos;
        t->files[i].hdr_pos = cur_hdr;
        t->files[i].hdr_bytes = file_hdr_len(i, t->files[i].size, 0, t->files[i].name);
        cur_pos += t->files[i].size;
        cur_hdr += t->files[i].hdr_bytes;
    }
    t->total_bytes = cur_pos;
    t->total_hdr_bytes = cur_hdr;
    return 0;
}

int topic_map_build(topic_map_t *map, const char *root_dir) {
    char abs_root[PATH_MAX];
    if (!realpath(root_dir, abs_root)) {
        log_msg(LOG_ERR, "Failed to resolve root dir %s: %s", root_dir, strerror(errno));
        return -1;
    }

    DIR *d = opendir(abs_root);
    if (!d) {
        log_msg(LOG_ERR, "Failed to open topics root dir %s: %s", abs_root, strerror(errno));
        return -1;
    }

    size_t cap = 16;
    map->topics = malloc(cap * sizeof(topic_t));
    if (!map->topics) {
        closedir(d);
        return -1;
    }
    map->count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strchr(de->d_name, '\n') != NULL) continue;

        char subpath[PATH_MAX * 2];
        if ((size_t)snprintf(subpath, sizeof(subpath), "%s/%s", abs_root, de->d_name) >= sizeof(subpath)) continue;

        struct stat st;
        if (stat(subpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (map->count >= cap) {
            cap *= 2;
            topic_t *nt = realloc(map->topics, cap * sizeof(topic_t));
            if (!nt) {
                closedir(d);
                return -1;
            }
            map->topics = nt;
        }

        topic_t *top = &map->topics[map->count];
        size_t name_len = strlen(de->d_name);
        size_t path_len = strlen(subpath);
        if (name_len >= sizeof(top->key) || path_len >= sizeof(top->path)) continue;
        memcpy(top->key, de->d_name, name_len + 1);
        memcpy(top->path, subpath, path_len + 1);

        if (scan_topic_directory(top, subpath) == 0) {
            map->count++;
        }
    }
    closedir(d);

    if (map->count > 0) {
        qsort(map->topics, map->count, sizeof(topic_t), compare_topics);
    }
    log_msg(LOG_INFO, "Indexed %u topics from %s", map->count, abs_root);
    return 0;
}

const topic_t *topic_lookup(const topic_map_t *map, const char *key) {
    if (!map || !key || key[0] == '\0') return NULL;
    if (strlen(key) >= 64) return NULL;
    /* Path traversal check */
    if (strchr(key, '/') != NULL || strstr(key, "..") != NULL) {
        return NULL;
    }

    /* Exact match first */
    for (uint32_t i = 0; i < map->count; i++) {
        if (strcmp(map->topics[i].key, key) == 0) {
            return &map->topics[i];
        }
    }

    /* Case-insensitive match */
    for (uint32_t i = 0; i < map->count; i++) {
        if (strcasecmp(map->topics[i].key, key) == 0) {
            return &map->topics[i];
        }
    }

    return NULL;
}

/*
 * Projects a (file index, offset) tuple onto the continuous payload
 * position. This removes the boundary ambiguity between "file i at its
 * full size" and "file i+1 at offset 0" (Design Document Section 5.4).
 */
uint64_t topic_pos(const topic_t *topic, uint32_t file_idx, uint64_t offset) {
    if (!topic) return 0;
    if (file_idx >= topic->file_count) return topic->total_bytes;
    return topic->files[file_idx].pos + offset;
}

/* Payload bytes of the range [first_idx..last_idx], honouring an offset. */
uint64_t topic_stream_size(const topic_t *topic, uint32_t first_idx, uint32_t last_idx, uint64_t offset) {
    if (!topic || first_idx >= topic->file_count || last_idx >= topic->file_count || first_idx > last_idx) {
        return 0;
    }
    uint64_t total = 0;
    for (uint32_t i = first_idx; i <= last_idx; i++) {
        uint64_t s = topic->files[i].size;
        if (i == first_idx) {
            if (offset < s) {
                total += (s - offset);
            }
        } else {
            total += s;
        }
    }
    return total;
}

/*
 * Header bytes for the range [first_idx..n-1]. The header of first_idx is
 * recomputed with the real start offset, all later headers come from the
 * precomputed table. O(1) in the number of files.
 */
uint64_t topic_hdr_size(const topic_t *topic, uint32_t first_idx, uint64_t first_off) {
    if (!topic || first_idx >= topic->file_count) return 0;
    uint64_t first_hdr = file_hdr_len(first_idx, topic->files[first_idx].size, first_off,
                                      topic->files[first_idx].name);
    uint64_t rest = topic->total_hdr_bytes - topic->files[first_idx].hdr_pos - topic->files[first_idx].hdr_bytes;
    return first_hdr + rest;
}

/* Payload bytes remaining from (first_idx, offset) to the end of the topic. */
uint64_t topic_stream_size_remaining(const topic_t *topic, uint32_t first_idx, uint64_t offset) {
    if (!topic) return 0;
    if (first_idx >= topic->file_count) return 0;
    uint64_t total = topic->total_bytes - topic->files[first_idx].pos;
    if (offset > topic->files[first_idx].size) return 0;
    return total - offset;
}

void topic_map_free(topic_map_t *map) {    if (!map) return;
    for (uint32_t i = 0; i < map->count; i++) {
        free(map->topics[i].files);
    }
    free(map->topics);
    map->topics = NULL;
    map->count = 0;
}
