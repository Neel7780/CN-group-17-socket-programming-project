#ifndef SESSION_H
#define SESSION_H

#include "common.h"
#include "topics.h"
#include "proto.h"

/*
 * Server-side session control block.
 *
 * Design Document Section 5.3 (Listing 2). One session_t is allocated per
 * client transfer that begins with a GET. The session survives connection
 * loss: when a fault aborts a transfer the descriptor is deliberately left
 * in the table so the client can reconnect and resume from its checkpoint.
 *
 * Locking:
 *   g_table_mu  guards the bucket arrays and the lifetime refcount (lrefs).
 *   s->mu       guards every progress field below.
 * Lock order is always table -> session; never the reverse.
 */
typedef struct session {
    char sid[33];                   /* 128-bit hex ID from /dev/urandom */
    const topic_t *topic;           /* Pointer to immutable topic metadata */
    pthread_mutex_t mu;             /* Mutex guarding progress fields below */
    uint64_t hw_pos;                /* High-water mark in payload stream */
    uint64_t bytes_sent;            /* Cumulative bytes across attempts */
    uint32_t attempts;              /* Total connection attempts */
    uint32_t generation;            /* Incremented on each RESUME */
    uint32_t refs;                  /* Active connection count */
    time_t last_seen;               /* Timestamp of most recent activity */
    struct session *next;           /* Hash bucket collision chain */
    uint32_t lrefs;                 /* Lifetime refcount, guarded by g_table_mu */
} session_t;

/* Table lifecycle. ttl is the session time-to-live in seconds. */
void session_table_init(long ttl);
void session_table_set_ttl(long ttl);
long session_table_get_ttl(void);

session_t *session_create(const topic_t *topic);

/* Looks up a session and takes a reference on it. Returns NULL if unknown. */
session_t *session_lookup(const char *sid);

/* Drops a reference obtained from session_lookup(). */
void session_release(session_t *s);

/* Removes the session from the table (used on DONE). Reference safe. */
void session_destroy(const char *sid);

void session_update_activity(session_t *s);
uint32_t session_generation(session_t *s);

/* Advances the high-water mark; pos is a payload-space offset. */
void session_update_hw(session_t *s, uint64_t pos);
void session_add_bytes_sent(session_t *s, uint64_t bytes);

/*
 * Validates a RESUME request against the session.
 * Returns 0 when the checkpoint is accepted, otherwise the protocol error
 * code to report: 409 (topic mismatch), 410 (session expired) or
 * 416 (bad checkpoint). On success the session generation is incremented
 * and *rem_out receives the number of payload bytes still to be delivered.
 */
int session_validate_resume(session_t *s, const char *topic, uint32_t file_idx,
                             uint64_t offset, uint64_t *rem_out);

void *session_reaper_thread(void *arg);
void session_reaper_stop(void);

#endif /* SESSION_H */
