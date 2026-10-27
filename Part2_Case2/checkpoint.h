#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "common.h"

/*
 * Client-side durable checkpoint.
 *
 * Design Document Section 5.2 / Table 5. The on-disk format is a single
 * line:
 *
 *     v1 <sid> <topic> <idx> <off> <done>
 *
 * where <idx> is the index of the file currently being received, <off> is
 * the number of bytes of that file that are durably on disk, and <done> is
 * the number of files that have been completely received and renamed.
 *
 * A checkpoint of (idx == file_count, off == 0) means every file is already
 * on disk and only the trailing END delimiter was lost.
 *
 * Atomicity: the record is written to .ckpt.tmp, fsync'ed, then rename()d
 * over .ckpt. rename() is atomic on POSIX filesystems, so a crash can never
 * leave a torn checkpoint behind.
 */
typedef struct {
    char sid[33];
    char topic[64];
    char dir[PATH_MAX];
    uint32_t file_count;
    uint32_t cur_idx;      /* Index of the file being received */
    uint64_t cur_off;      /* Durable bytes of cur_idx on disk */
    uint32_t done;         /* Number of fully completed files */
    pthread_mutex_t mu;
} ckpt_t;

/* Creates an empty checkpoint for the given topic directory. */
ckpt_t *ckpt_create(const char *topic_dir, const char *topic, const char *sid, uint32_t file_count);

/* Loads the checkpoint from disk. Returns 0 on success, -1 if absent/invalid. */
int ckpt_load(ckpt_t *c);

/* Atomically persists the checkpoint. */
int ckpt_save_atomic(ckpt_t *c);

/* Records progress inside the current file. */
void ckpt_set_progress(ckpt_t *c, uint32_t file_idx, uint64_t offset);

/* Records that a file has been fully received. */
void ckpt_mark_complete(ckpt_t *c, uint32_t file_idx);

/* True when the given file has been fully received. */
bool ckpt_is_complete(ckpt_t *c, uint32_t file_idx);

/* Deletes the checkpoint files. */
void ckpt_remove(ckpt_t *c);

void ckpt_free(ckpt_t *c);

#endif /* CHECKPOINT_H */
