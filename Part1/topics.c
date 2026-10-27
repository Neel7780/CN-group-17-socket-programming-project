#define _GNU_SOURCE
#include "topics.h"
#include <dirent.h>

static int compare_files(const void *a, const void *b) {
    return strcmp(((const file_entry_t *)a)->name,
                  ((const file_entry_t *)b)->name);
}

static int compare_topics(const void *a, const void *b) {
    return strcmp(((const topic_t *)a)->key, ((const topic_t *)b)->key);
}

/* Scans one topic sub-directory and populates t->files. */
static int scan_topic_dir(topic_t *t, const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    size_t cap = 32;
    t->files = malloc(cap * sizeof(file_entry_t));
    if (!t->files) { closedir(d); return -1; }
    t->file_count = 0;
    t->total_bytes = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;            /* skip hidden */
        if (strchr(de->d_name, '\n') != NULL) continue; /* keep headers single-line */

        char fpath[PATH_MAX * 2];
        if ((size_t)snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, de->d_name)
                >= sizeof(fpath)) continue;

        struct stat st;
        if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (t->file_count >= cap) {
            cap *= 2;
            file_entry_t *nf = realloc(t->files, cap * sizeof(file_entry_t));
            if (!nf) { closedir(d); return -1; }
            t->files = nf;
        }

        file_entry_t *fe = &t->files[t->file_count++];
        snprintf(fe->name, sizeof(fe->name), "%s", de->d_name);
        fe->size = (uint64_t)st.st_size;
    }
    closedir(d);

    /* Sort files alphabetically for a deterministic, permanent index. */
    if (t->file_count > 0)
        qsort(t->files, t->file_count, sizeof(file_entry_t), compare_files);

    for (uint32_t i = 0; i < t->file_count; i++)
        t->total_bytes += t->files[i].size;

    return 0;
}

int topic_map_build(topic_map_t *map, const char *root_dir) {
    char abs_root[PATH_MAX];
    if (!realpath(root_dir, abs_root)) {
        log_msg(LOG_ERR, "Cannot resolve topics root dir %s: %s", root_dir, strerror(errno));
        return -1;
    }

    DIR *d = opendir(abs_root);
    if (!d) {
        log_msg(LOG_ERR, "Cannot open topics root dir %s: %s", abs_root, strerror(errno));
        return -1;
    }

    size_t cap = 16;
    map->topics = malloc(cap * sizeof(topic_t));
    if (!map->topics) { closedir(d); return -1; }
    map->count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strchr(de->d_name, '\n') != NULL) continue;

        char subpath[PATH_MAX * 2];
        if ((size_t)snprintf(subpath, sizeof(subpath), "%s/%s", abs_root, de->d_name)
                >= sizeof(subpath)) continue;

        struct stat st;
        if (stat(subpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* Topic key must satisfy the same constraints clients may send */
        size_t name_len = strlen(de->d_name);
        if (name_len > MAX_TOPIC_LEN) continue;

        if (map->count >= cap) {
            cap *= 2;
            topic_t *nt = realloc(map->topics, cap * sizeof(topic_t));
            if (!nt) { closedir(d); return -1; }
            map->topics = nt;
        }

        topic_t *top = &map->topics[map->count];
        memcpy(top->key, de->d_name, name_len + 1);
        memcpy(top->path, subpath, strlen(subpath) + 1);
        top->files = NULL;
        top->file_count = 0;
        top->total_bytes = 0;

        if (scan_topic_dir(top, subpath) == 0) {
            map->count++;
        } else {
            log_msg(LOG_WARN, "Skipping topic '%s': scan failed", de->d_name);
        }
    }
    closedir(d);

    if (map->count > 0)
        qsort(map->topics, map->count, sizeof(topic_t), compare_topics);

    log_msg(LOG_INFO, "Indexed %u topics from %s", map->count, abs_root);
    return 0;
}

const topic_t *topic_lookup(const topic_map_t *map, const char *key) {
    if (!map || !key || key[0] == '\0') return NULL;
    if (strlen(key) > MAX_TOPIC_LEN) return NULL;
    if (strchr(key, '/') || strstr(key, "..")) return NULL;

    /* Exact match first. */
    for (uint32_t i = 0; i < map->count; i++)
        if (strcmp(map->topics[i].key, key) == 0) return &map->topics[i];

    /* Case-insensitive fallback (Design Document Section 2.2). */
    for (uint32_t i = 0; i < map->count; i++)
        if (strcasecmp(map->topics[i].key, key) == 0) return &map->topics[i];

    return NULL;
}

void topic_map_free(topic_map_t *map) {
    if (!map) return;
    for (uint32_t i = 0; i < map->count; i++)
        free(map->topics[i].files);
    free(map->topics);
    map->topics = NULL;
    map->count = 0;
}
