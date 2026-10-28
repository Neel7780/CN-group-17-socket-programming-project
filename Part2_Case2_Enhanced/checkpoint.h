#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "common.h"

typedef struct {
    char sid[33];
    char topic[64];
    char dir[PATH_MAX];
    uint32_t file_count;
    uint8_t *mask;         /* Bitmask: 1 bit per file */
    pthread_mutex_t mu;
} ckpt_t;

ckpt_t *ckpt_create(const char *topic_dir, const char *topic, const char *sid, uint32_t file_count);
int ckpt_load(ckpt_t *c);
int ckpt_save_atomic(ckpt_t *c);
void ckpt_mark_complete(ckpt_t *c, uint32_t file_idx);
bool ckpt_is_complete(ckpt_t *c, uint32_t file_idx);
bool ckpt_all_complete(ckpt_t *c);
void ckpt_remove(ckpt_t *c);
void ckpt_free(ckpt_t *c);

#endif /* CHECKPOINT_H */
