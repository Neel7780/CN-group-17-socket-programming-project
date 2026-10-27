#ifndef METRICS_H
#define METRICS_H

#include "common.h"

void metrics_init(const char *csv_path);
void metrics_log_row(const char *role, const char *event, const char *sid, uint32_t conn_id,
                     const char *topic, uint32_t file_idx, uint64_t bytes,
                     uint64_t duration_ms, const char *status);
void metrics_close(void);

#endif /* METRICS_H */
