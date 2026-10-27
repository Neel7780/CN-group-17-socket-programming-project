#define _GNU_SOURCE
#include "checkpoint.h"

ckpt_t *ckpt_create(const char *topic_dir, const char *topic, const char *sid, uint32_t file_count) {
    if (!topic_dir || !topic) return NULL;

    ckpt_t *c = calloc(1, sizeof(ckpt_t));
    if (!c) return NULL;

    snprintf(c->dir, sizeof(c->dir), "%s", topic_dir);
    snprintf(c->topic, sizeof(c->topic), "%s", topic);
    if (sid) snprintf(c->sid, sizeof(c->sid), "%s", sid);
    c->file_count = file_count;
    c->cur_idx = 0;
    c->cur_off = 0;
    c->done = 0;
    pthread_mutex_init(&c->mu, NULL);
    return c;
}

int ckpt_save_atomic(ckpt_t *c) {
    if (!c) return -1;

    char tmp_path[PATH_MAX * 2];
    char final_path[PATH_MAX * 2];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.ckpt.tmp", c->dir);
    snprintf(final_path, sizeof(final_path), "%s/.ckpt", c->dir);

    pthread_mutex_lock(&c->mu);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    fprintf(f, "v1 %s %s %u %" PRIu64 " %u\n",
            c->sid, c->topic, c->cur_idx, c->cur_off, c->done);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    /* rename() is atomic: readers see either the old or the new record. */
    if (rename(tmp_path, final_path) != 0) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    pthread_mutex_unlock(&c->mu);
    return 0;
}

int ckpt_load(ckpt_t *c) {
    if (!c) return -1;

    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/.ckpt", c->dir);

    pthread_mutex_lock(&c->mu);

    FILE *f = fopen(path, "r");
    if (!f) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    char version[16];
    char sid[33];
    char topic[64];
    uint32_t cur_idx = 0;
    uint64_t cur_off = 0;
    uint32_t done = 0;

    if (fscanf(f, "%15s %32s %63s %u %" SCNu64 " %u", version, sid, topic,
               &cur_idx, &cur_off, &done) != 6) {
        fclose(f);
        pthread_mutex_unlock(&c->mu);
        return -1;
    }
    fclose(f);

    if (strcmp(version, "v1") != 0) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }
    /* The checkpoint must belong to this topic. */
    if (strcmp(topic, c->topic) != 0) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }
    if (cur_idx > c->file_count || done > c->file_count) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    snprintf(c->sid, sizeof(c->sid), "%s", sid);
    c->cur_idx = cur_idx;
    c->cur_off = cur_off;
    c->done = done;

    pthread_mutex_unlock(&c->mu);

    log_msg(LOG_INFO, "Checkpoint loaded: sid=%s, idx=%u, off=%" PRIu64 ", done=%u",
            c->sid, cur_idx, cur_off, done);
    return 0;
}

void ckpt_set_progress(ckpt_t *c, uint32_t file_idx, uint64_t offset) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->cur_idx = file_idx;
    c->cur_off = offset;
    pthread_mutex_unlock(&c->mu);
    ckpt_save_atomic(c);
}

void ckpt_mark_complete(ckpt_t *c, uint32_t file_idx) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->cur_idx = file_idx + 1;
    c->cur_off = 0;
    if (c->done <= file_idx) {
        c->done = file_idx + 1;
    }
    pthread_mutex_unlock(&c->mu);
    ckpt_save_atomic(c);
}

bool ckpt_is_complete(ckpt_t *c, uint32_t file_idx) {
    if (!c) return false;
    pthread_mutex_lock(&c->mu);
    bool done = (file_idx < c->done);
    pthread_mutex_unlock(&c->mu);
    return done;
}

void ckpt_remove(ckpt_t *c) {
    if (!c) return;
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/.ckpt", c->dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.ckpt.tmp", c->dir);
    unlink(path);
}

void ckpt_free(ckpt_t *c) {
    if (!c) return;
    pthread_mutex_destroy(&c->mu);
    free(c);
}
