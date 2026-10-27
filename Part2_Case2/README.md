# Part II — Case 2: Fault Tolerant Transfer with Session Management

Implements the session table and resume protocol described in
**Design Document Section 5** of `Design_Document_Group_021_034_.pdf`.

The server and the client both retain state across a connection loss. When
the injected fault severs the connection, the client reconnects and reports
its last durable checkpoint; the server validates that checkpoint against
its own high-water mark and resumes streaming from the corresponding byte
offset instead of restarting the topic from the first byte.

---

## Build

```sh
make            # produces ./server and ./client
make clean
```

Requires a C11 compiler and POSIX threads. `gcc -std=c11 -O2 -Wall -Wextra
-Werror -pthread` is used; the code builds warning-free.

## Run

```sh
# Server (assignment-mandated interface)
./server <port> <topics_root_dir> <failure_probability>

# Client (assignment-mandated interface)
./client <server_ip> <server_port> <topic> <output_dir>
```

The client writes the topic's files to `<output_dir>/<topic>/`.

### Optional trailing arguments

| Program | Flags | Default | Meaning |
|---|---|---|---|
| server | `seed=<n>` | 42 | Seed for the fault random stream (reproducible experiments) |
| server | `session_ttl=<n>` | 300 | Seconds before an idle session is reaped |
| server | `max_clients=<n>` | 128 | Maximum concurrent client threads |
| server | `sendfile=<0\|1>` | 1 | Use zero-copy `sendfile()` instead of `read()`/`send()` |
| server | `log=<0..3>` | 1 | Verbosity: 0=errors, 1=info, 2=debug |
| client | `ckpt_kb=<n>` | 64 | Intra-file checkpoint interval in KiB |
| client | `max_retries=<n>` | 50 | Maximum connection attempts |
| client | `backoff_ms=<n>` | 100 | Initial retry backoff (doubles up to `backoff_max_ms`) |
| client | `backoff_max_ms=<n>` | 2000 | Backoff ceiling |
| client | `log=<0..3>` | 1 | Verbosity |

Every flag is optional; the two mandated invocations above work unchanged.

---

## Protocol (Design Document Section 3.4 / Table 3)

All control messages are single ASCII lines terminated by `\n`. File
contents follow their header as raw binary whose exact length is given by
the header.

| Direction | Message | Meaning |
|---|---|---|
| C → S | `GET <topic>` | Start a new resumable session |
| S → C | `SESSION <sid> <file count> <total bytes>` | Session id and topic totals |
| C → S | `RESUME <sid> <topic> <file index> <offset>` | Resume from the client's checkpoint |
| S → C | `RESUMED <file index> <offset> <remaining bytes>` | Checkpoint accepted, resuming here |
| S → C | `FILE <index> <size> <start> <name>` + `(size-start)` raw bytes | One file |
| S → C | `END <file count> <total bytes>` | Topic stream complete |
| C → S | `DONE <sid>` | Client confirms everything is flushed to disk |
| S → C | `BYE` | Server releases the session |
| S → C | `ERR <code> <REASON>` | `404` no such topic, `409` topic mismatch, `410` session expired, `416` bad checkpoint, `400` bad request |

`<start>` is `0` for whole files and equals the checkpoint offset for the
first file transmitted after a `RESUMED` response.

## Session table (Design Document Section 5.3)

`session.c` implements a 256-bucket hash table keyed by the 32-character hex
session id.

```c
typedef struct session {
    char sid[33];                   /* 128-bit id from /dev/urandom */
    const topic_t *topic;           /* immutable topic metadata */
    pthread_mutex_t mu;             /* guards the progress fields */
    uint64_t hw_pos;                /* high-water mark in payload space */
    uint64_t bytes_sent;            /* cumulative bytes across attempts */
    uint32_t attempts;              /* total connection attempts */
    uint32_t generation;            /* incremented on each RESUME */
    uint32_t refs;                  /* active connection count */
    time_t last_seen;               /* last activity timestamp */
    struct session *next;           /* bucket collision chain */
    uint32_t lrefs;                 /* lifetime refcount (table + lookups) */
} session_t;
```

* **Locking** — `g_table_mu` guards the buckets and the lifetime refcount;
  `s->mu` guards the progress fields. The lock order is always
  table → session, never the reverse.
* **Lifetime** — a session outlives the connection that created it. Dropping
  a connection decrements `refs` but leaves the session in the table, so the
  client can reconnect and resume. Only an explicit `DONE` or the TTL reaper
  removes it.
* **High-water mark** — `hw_pos` is advanced after every completed file and
  after every partial write caused by a fault. A TCP RST discards bytes
  still buffered in the kernel, so `hw_pos` is a safe *upper* bound on what
  the client could have received.

## Checkpoint validation (Design Document Section 5.4)

A checkpoint is a tuple `(file index, offset)`. Comparing raw tuples is
ambiguous at file boundaries — `(i, size_i)` and `(i+1, 0)` denote the same
position — so the tuple is projected onto the continuous payload position

```
pos(i, o) = topic->files[i].pos + o
```

and compared against `hw_pos`. The server accepts a `RESUME` if and only if:

1. the session id exists and has not expired — otherwise `410`;
2. the requested topic matches the session's bound topic — otherwise `409`;
3. the index and offset are in range **and** `pos(i, o) <= hw_pos` —
   otherwise `416`.

On acceptance the server increments `generation`, so a thread orphaned by an
earlier interrupted connection detects the change and exits instead of
transmitting concurrently on the same session.

## Client-side state (Design Document Section 5.2 / Table 5)

| State | Location | Update |
|---|---|---|
| Checkpoint record | `<output_dir>/<topic>/.ckpt` | After every complete file and every `ckpt_kb` within a partial file |
| Partial payload | `<output_dir>/<topic>/<name>.part` | Continuously; renamed to `<name>` on completion |
| Attempt counters | Process memory → `client_metrics.csv` | Continuously, flushed on completion |

The checkpoint record is one line:

```
v1 <sid> <topic> <idx> <off> <done>
```

`<idx>` is the file being received, `<off>` the durable byte offset within
it, `<done>` the number of completed files. A checkpoint of
`(idx == file count, off == 0)` means every file is already on disk and only
the trailing `END` was lost.

**Atomicity.** The record is written to `.ckpt.tmp`, `fsync`ed, and then
`rename()`d over `.ckpt`. `rename()` is atomic on POSIX filesystems, so a
crash can never leave a torn checkpoint.

**Rollback.** On reconnect the client `ftruncate()`s the partial file back
to the checkpointed offset, discarding bytes that were received but never
checkpointed. If a resume is rejected the client discards all partial files
and the checkpoint and falls back to a clean `GET`.

## Fault injection (Design Document Section 4)

For each request the server performs exactly **one** Bernoulli trial with
probability `p`. If it fires, a cut point `K` is drawn uniformly over the
total response bytes `R` (headers plus payload), computed in O(1) from the
precomputed per-file offsets, and the socket is aborted once `K` bytes have
been written:

```c
struct linger sl = { .l_onoff = 1, .l_linger = 0 };
setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
close(fd);                     /* emits a TCP RST */
```

`SIGPIPE` is ignored and writes use `MSG_NOSIGNAL`.

The random variate comes from a per-connection `erand48()` state derived
from `seed` and the connection id through a SplitMix64 avalanche function.
Without that mixing the raw words are correlated and the fault decision of
consecutive connections behaves systematically instead of following the
requested Bernoulli distribution.

`p` is a per-*request* probability, not a per-chunk error rate, so the
expected number of attempts to completion is `1/(1-p)` and a transfer
remains tractable at every `p`.

## Error handling

| Situation | Behaviour |
|---|---|
| Premature EOF / RST mid-stream | Client checkpoints, reconnects, issues `RESUME` |
| `RESUME` rejected (409/410/416) | Client discards partial state and restarts with `GET` |
| `GET` for an unknown topic | Server replies `ERR 404`, client exits 3 |
| Malformed request line | Server replies `ERR 400`, client exits 4 |
| Retries exhausted | Client exits 2 |
| Success | Client exits 0, checkpoint removed, `DONE`/`BYE` exchanged |

The `DONE`/`BYE` exchange is best-effort: if the connection drops before
`BYE` the transfer is still complete, and the reaper reclaims the orphaned
session after the TTL expires.

## Files

| File | Role |
|---|---|
| `session.c/.h` | Session table, refcounting, checkpoint validation, TTL reaper |
| `checkpoint.c/.h` | Client-side durable checkpoint (atomic write, rollback) |
| `proto.c/.h` | Case 2 message parsing and formatting |
| `server.c` | Accept loop, `GET`/`RESUME`/`DONE` handling, worker thread |
| `client.c` | Connect, receive, checkpoint, resume loop, fallback |
| `transfer.c` | Streams the topic payload and advances the high-water mark |
| `fault.c` | Per-request fault plan and the cut-point budget |
| `topics.c` | Topic indexing, deterministic file order, O(1) offset translation |
| `common.c` | Buffered line reader, socket helpers, portability shims |
| `metrics.c` | CSV performance rows |

## Testing

```sh
./test_case2.sh [port] [topic] [failure_probability]
./validate_case2.sh [runs_per_fault_rate]
```

`test_case2.sh` downloads a topic and verifies every file by SHA-256.
`validate_case2.sh` sweeps `p ∈ {0.0, 0.2, 0.5, 0.8}` and checks, for each
run, that the client succeeds, that no `.part` or `.ckpt` files are left
behind, and that every file matches the source byte for byte.

## Portability note

The project targets Linux, where `sendfile(2)` provides the zero-copy
transfer path. `common.c` wraps it in `sock_sendfile_chunk()`: Linux uses
the native call, while other platforms fall back to an equivalent
`pread()`/`send()` loop. The fallback keeps the caller's file offset
private until the transfer succeeds, so a failed `sendfile()` cannot
disturb the offset and cause the same block to be sent repeatedly.
