#define _GNU_SOURCE
#include "workq.h"

workq_t *workq_create(void) {
    workq_t *q = calloc(1, sizeof(workq_t));
    if (!q) return NULL;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->closed = false;
    q->count = 0;
    q->uncompleted_tasks = 0;
    return q;
}

void workq_push_back(workq_t *q, uint32_t first_idx, uint32_t last_idx, uint64_t offset) {
    batch_task_t *t = calloc(1, sizeof(batch_task_t));
    if (!t) return;
    t->first_idx = first_idx;
    t->last_idx = last_idx;
    t->offset = offset;

    pthread_mutex_lock(&q->mu);
    if (!q->tail) {
        q->head = q->tail = t;
    } else {
        q->tail->next = t;
        q->tail = t;
    }
    q->count++;
    q->uncompleted_tasks++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

void workq_push_front(workq_t *q, uint32_t first_idx, uint32_t last_idx, uint64_t offset) {
    batch_task_t *t = calloc(1, sizeof(batch_task_t));
    if (!t) return;
    t->first_idx = first_idx;
    t->last_idx = last_idx;
    t->offset = offset;

    pthread_mutex_lock(&q->mu);
    t->next = q->head;
    q->head = t;
    if (!q->tail) {
        q->tail = t;
    }
    q->count++;
    /* Note: uncompleted_tasks is not incremented because this is a re-enqueued unfinished task */
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

int workq_pop(workq_t *q, batch_task_t *out_task) {
    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed) {
        if (q->uncompleted_tasks == 0) {
            q->closed = true;
            pthread_cond_broadcast(&q->cv);
            break;
        }
        pthread_cond_wait(&q->cv, &q->mu);
    }

    if (q->head) {
        batch_task_t *t = q->head;
        q->head = t->next;
        if (!q->head) q->tail = NULL;
        q->count--;

        *out_task = *t;
        out_task->next = NULL;
        free(t);
        pthread_mutex_unlock(&q->mu);
        return 1;
    }

    pthread_mutex_unlock(&q->mu);
    return 0;
}

void workq_task_done(workq_t *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    if (q->uncompleted_tasks > 0) {
        q->uncompleted_tasks--;
    }
    if (!q->head && q->uncompleted_tasks == 0) {
        q->closed = true;
        pthread_cond_broadcast(&q->cv);
    } else {
        pthread_cond_signal(&q->cv);
    }
    pthread_mutex_unlock(&q->mu);
}

void workq_close(workq_t *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

void workq_destroy(workq_t *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    batch_task_t *curr = q->head;
    while (curr) {
        batch_task_t *nxt = curr->next;
        free(curr);
        curr = nxt;
    }
    q->head = q->tail = NULL;
    pthread_mutex_unlock(&q->mu);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
    free(q);
}
