# Fault Tolerant Topic-based File Distribution Service

IT305 Socket Programming Project, Group 17 (lab groups 021 and 034).

## Part I

### Build

```sh
cd Part1
make          # builds ./client (and ./server once server.c is present)
make test     # unit tests for the line reader, protocol parsers and metrics logger
```

### Client

```sh
./client <server_ip> <server_port> <topic> <output_dir> [log=0..3] [timeout=SEC] [metrics=PATH] [tag=LABEL]
```

Sends `GET <topic>` and writes every file of the topic to `<output_dir>/<topic>/`.
Each file is received into `<name>.part` and renamed once complete, so an
interrupted transfer never leaves a truncated file under its real name.

| Option    | Default              | Meaning |
|-----------|----------------------|---------|
| `log`     | `2`                  | stderr verbosity: 0 error, 1 warn, 2 info, 3 debug |
| `timeout` | `0` (none)           | receive timeout in seconds; keep 0 against the single-threaded server, where queued clients wait their turn |
| `metrics` | `client_metrics.csv` | CSV file that gets one row per run; `none` disables it |
| `tag`     | `-`                  | label written to the `mode` column, e.g. `single_n8` |

Exit codes: `0` success (END received and matches OK), `1` usage error,
`2` transfer did not complete (connect failure, premature EOF, protocol error),
`3` topic not found / rejected, `4` local I/O error.

### Metrics CSV

Client and server share one row format:

```
wall_start_ns,wall_end_ns,role,mode,conn_id,topic,files,payload_bytes,wire_bytes,wait_ms,duration_ms,throughput_mbps,status
```

For the client, `wait_ms` is the time from `connect()` to the `OK` line
(it includes any queueing in a single-threaded server). `duration_ms` is the time from `OK` to `END`.
The wall-clock columns are epoch nanoseconds, so rows from several client processes can be
combined. For N concurrent clients, the aggregate transfer rate is
`sum(payload_bytes) / (max(wall_end_ns) - min(wall_start_ns))`.
Concurrent clients may safely append to the same file.
