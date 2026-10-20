#ifndef WORKQ_H
#define WORKQ_H

#include "common.h"

typedef struct batch_task {
    uint32_t first_idx;
    uint32_t last_idx;
    uint64_t offset;
    struct batch_task *next;
} batch_task_t;

typedef struct {
    batch_task_t *head;
    batch_task_t *tail;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool closed;
    size_t count;
    size_t uncompleted_tasks;
} workq_t;

workq_t *workq_create(void);
void workq_push_back(workq_t *q, uint32_t first_idx, uint32_t last_idx, uint64_t offset);
void workq_push_front(workq_t *q, uint32_t first_idx, uint32_t last_idx, uint64_t offset);
int workq_pop(workq_t *q, batch_task_t *out_task);
void workq_task_done(workq_t *q);
void workq_close(workq_t *q);
void workq_destroy(workq_t *q);

#endif /* WORKQ_H */
