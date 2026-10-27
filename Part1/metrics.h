#ifndef METRICS_H
#define METRICS_H

#include "common.h"

/*
 * Per-transfer CSV metrics (Section 7, Table 8).
 *
 * One row is appended per completed (or failed) topic transfer:
 *
 *   wall_start_ns,wall_end_ns,role,mode,conn_id,topic,files,payload_bytes,
 *   wire_bytes,wait_ms,duration_ms,throughput_mb_s,status
 *
 * wall_start_ns/wall_end_ns are CLOCK_REALTIME epoch nanoseconds so rows from
 * several client processes on one node can be merged: the aggregate transfer
 * rate for N concurrent clients is sum(payload_bytes) / (max(wall_end_ns) -
 * min(wall_start_ns)). duration_ms is measured with CLOCK_MONOTONIC.
 *
 * Several processes may append to the same file: each row is a single
 * O_APPEND write(), and the header is written once under flock().
 */

typedef enum {
    METRICS_SYNC,   /* Caller's thread writes the row (client, single-threaded server) */
    METRICS_ASYNC   /* Rows are queued; a background logger thread writes them (multi-threaded server) */
} metrics_mode_t;

typedef struct {
    const char *role;          /* "server" or "client" */
    const char *mode;          /* Server: "single" / "multi"; client: experiment tag */
    uint64_t conn_id;          /* Server connection id, or client pid */
    const char *topic;
    uint32_t files;            /* Files fully transferred */
    uint64_t payload_bytes;    /* File content bytes */
    uint64_t wire_bytes;       /* All bytes on the socket: headers + payload */
    uint64_t wall_start_ns;    /* Start of the measured interval (now_wall_ns()) */
    uint64_t wait_ns;          /* Time before the measured interval (connect/queueing) */
    uint64_t duration_ns;      /* Length of the measured interval (now_ns() deltas) */
    const char *status;        /* "OK", "EOF", "ERR_404", ... */
} metrics_row_t;

/* Opens (or creates) the CSV. Returns 0, or -1 if the file cannot be opened; logging is then a no-op. */
int metrics_open(const char *csv_path, metrics_mode_t mode);

/* Records one row. Safe to call from any thread; never blocks on disk in METRICS_ASYNC mode. */
void metrics_row(const metrics_row_t *row);

/* Flushes queued rows, stops the logger thread, and closes the file. */
void metrics_close(void);

#endif /* METRICS_H */
