#ifndef SESSION_H
#define SESSION_H

#include "common.h"
#include "topics.h"

typedef struct session {
    char sid[33];                   /* 128-bit hex ID from /dev/urandom */
    const topic_t *topic;           /* Pointer to immutable topic metadata */
    pthread_mutex_t mu;             /* Mutex guarding progress fields below */
    uint64_t hw_pos;                /* High-water mark in payload stream */
    uint64_t bytes_sent;            /* Cumulative bytes across attempts */
    uint32_t attempts;              /* Total connection attempts */
    uint32_t generation;            /* Incremented on each RESUME */
    uint32_t refs;                  /* Active connection count (Enhanced) */
    time_t last_seen;               /* Timestamp of most recent activity */
    struct session *next;           /* Hash bucket collision chain */
} session_t;

void session_table_init(void);
session_t *session_create(const topic_t *topic);
session_t *session_lookup(const char *sid);
void session_release(session_t *s);
void session_destroy(const char *sid);
void session_update_activity(session_t *s);
int session_validate_resume(session_t *s, const char *topic, uint32_t file_idx, uint64_t offset, uint64_t *rem_out);

void *session_reaper_thread(void *arg);
void session_reaper_stop(void);

#endif /* SESSION_H */
