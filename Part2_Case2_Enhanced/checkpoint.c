#define _GNU_SOURCE
#include "checkpoint.h"

ckpt_t *ckpt_create(const char *topic_dir, const char *topic, const char *sid, uint32_t file_count) {
    ckpt_t *c = calloc(1, sizeof(ckpt_t));
    if (!c) return NULL;
    snprintf(c->dir, sizeof(c->dir), "%s", topic_dir);
    snprintf(c->topic, sizeof(c->topic), "%s", topic);
    if (sid) snprintf(c->sid, sizeof(c->sid), "%s", sid);
    c->file_count = file_count;
    size_t mbytes = (file_count + 7) / 8;
    if (mbytes == 0) mbytes = 1;
    c->mask = calloc(mbytes, 1);
    pthread_mutex_init(&c->mu, NULL);
    return c;
}

int ckpt_save_atomic(ckpt_t *c) {
    if (!c) return -1;
    pthread_mutex_lock(&c->mu);

    char tmp_path[PATH_MAX * 2];
    char final_path[PATH_MAX * 2];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.ckpt.tmp", c->dir);
    snprintf(final_path, sizeof(final_path), "%s/.ckpt", c->dir);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    fprintf(f, "v1 %s %s %u\n", c->sid, c->topic, c->file_count);
    size_t mbytes = (c->file_count + 7) / 8;
    for (size_t i = 0; i < mbytes; i++) {
        fprintf(f, "%02x", c->mask[i]);
    }
    fprintf(f, "\n");
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    if (rename(tmp_path, final_path) != 0) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    pthread_mutex_unlock(&c->mu);
    return 0;
}

int ckpt_load(ckpt_t *c) {
    if (!c) return -1;
    pthread_mutex_lock(&c->mu);

    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/.ckpt", c->dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    char version[16];
    char sid[33];
    char topic[64];
    uint32_t count = 0;
    if (fscanf(f, "%15s %32s %63s %u\n", version, sid, topic, &count) != 4) {
        fclose(f);
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    if (strcmp(version, "v1") != 0 || count != c->file_count) {
        fclose(f);
        pthread_mutex_unlock(&c->mu);
        return -1;
    }

    snprintf(c->sid, sizeof(c->sid), "%s", sid);
    size_t mbytes = (c->file_count + 7) / 8;
    for (size_t i = 0; i < mbytes; i++) {
        unsigned int b = 0;
        if (fscanf(f, "%02x", &b) == 1) {
            c->mask[i] = (uint8_t)b;
        }
    }
    fclose(f);

    pthread_mutex_unlock(&c->mu);
    return 0;
}

void ckpt_mark_complete(ckpt_t *c, uint32_t file_idx) {
    if (!c || file_idx >= c->file_count) return;
    pthread_mutex_lock(&c->mu);
    c->mask[file_idx / 8] |= (1 << (file_idx % 8));
    pthread_mutex_unlock(&c->mu);
    ckpt_save_atomic(c);
}

bool ckpt_is_complete(ckpt_t *c, uint32_t file_idx) {
    if (!c || file_idx >= c->file_count) return false;
    pthread_mutex_lock(&c->mu);
    bool done = (c->mask[file_idx / 8] & (1 << (file_idx % 8))) != 0;
    pthread_mutex_unlock(&c->mu);
    return done;
}

void ckpt_remove(ckpt_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/.ckpt", c->dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.ckpt.tmp", c->dir);
    unlink(path);
    pthread_mutex_unlock(&c->mu);
}

void ckpt_free(ckpt_t *c) {
    if (!c) return;
    pthread_mutex_destroy(&c->mu);
    free(c->mask);
    free(c);
}
