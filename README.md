# Fault Tolerant Topic-based File Distribution Service

**Course:** IT305: Computer Networks Lab — Socket Programming Project  
**Team:** Group 17 (Merged Lab Groups 021 & 034)  
**Authors:**
- **Aaditya Kaushik** (202401001, Group 034) — Part I server: topic-directory map, single/multi-threaded request dispatch
- **Nisarg Trivedi** (202401129, Group 034) — Part I client: buffered line reader, atomic file reassembly, metrics logger
- **Niranjan Dangi** (202401042, Group 034) — Fault injection subsystem (SO_LINGER TCP RST) and Part II Case 1 retry loop
- **Kavish Inani** (202401089, Group 021) — Part II Case 2: 256-bucket session table, high-water mark validation, resume protocol
- **Neel Khatri** (202401093, Group 021) — Part II Case 2 Enhanced: dynamic work queue, N=4 parallel TCP workers, sendfile
- **Manal Patel** (202401112, Group 021) — Automated benchmark harnesses, SHA-256 verifier, plot engine, final report integration

---

## 1. Project Overview & Directory Structure

This project implements a high-performance, fault-tolerant topic-based file distribution service using TCP sockets in C11 and POSIX threads. The service supports single/multi-threaded dispatch, deliberate probabilistic network fault injection with ungraceful TCP resets, session-state management with durable atomic checkpointing, and dynamic multi-socket parallel pipelining.

```
.
├── Makefile                               # Top-level unified build & packaging Makefile
├── FinalReport_Group_021_034.pdf          # Consolidated 6-page comprehensive project report
├── README.md                              # Complete build, run, and evaluation documentation
├── Part1/                                 # Baseline single & multi-threaded topic server & client
├── Part2_Case1/                           # Fault injection & naive restart retry loop
├── Part2_Case2/                           # 256-bucket session table & atomic resumable checkpointing
├── Part2_Case2_Enhanced/                  # Dynamic task queue, N=4 parallel TCP workers, zero-copy
├── Results/                               # Benchmark CSV datasets and publication-quality plots
│   ├── Part1_SingleThread_throughput.[csv|png]
│   ├── Part1_MultiThread_throughput.[csv|png]
│   ├── Part2_Case1_throughput.[csv|png]
│   ├── Part2_Case1_overhead.png
│   ├── Part2_Case2_throughput.[csv|png]
│   ├── Part2_Case2_overhead.png
│   ├── Part2_Case2Enhanced_throughput.[csv|png]
│   ├── Part2_Case2Enhanced_overhead.png
│   └── Part2_Comparison_throughput.png
└── scripts/                               # Automated benchmark and verification harnesses
    ├── run_part1.sh                       # Part I client scalability suite (N=1..16)
    ├── run_part2.sh                       # Part II fault-sweep benchmark (p=0.0..0.8)
    ├── verify.sh                          # End-to-end SHA-256 hash & leftover verifier
    ├── verify_fault_recovery.sh           # Automated multi-case fault recovery test suite
    └── plot.py                            # Matplotlib publication chart generator
```

---

## 2. Standard CLI Conformance

All executables adhere strictly to the assignment's automated grading interface:

| Variant | Server Invocation | Client Invocation |
|---|---|---|
| **Part 1** | `./server <port> <topics_root_dir>` | `./client <server_ip> <server_port> <topic> <output_dir>` |
| **Part 2 Case 1** | `./server <port> <topics_root_dir> <failure_probability>` | `./client <server_ip> <server_port> <topic> <output_dir>` |
| **Part 2 Case 2** | `./server <port> <topics_root_dir> <failure_probability>` | `./client <server_ip> <server_port> <topic> <output_dir>` |
| **Part 2 Case 2 Enhanced** | `./server <port> <topics_root_dir> <failure_probability>` | `./client <server_ip> <server_port> <topic> <output_dir>` |

*Optional trailing flags (e.g. `log=`, `seed=`, `conns=`, `max_retries=`) are supported with safe defaults so the required invocations run as-is.*

---

## 3. Quickstart & Build Instructions

### Unified Root Makefile
```sh
# Build all 4 variants
make all

# Run unit tests
make test

# Run automated multi-case fault recovery test suite (p=0.4)
make verify-faults

# Clean all binaries and build artifacts
make clean

# Package official submission archives (FinalSubmission_Group_021_034.zip & .tar.gz)
make package
```

You may also compile any individual part directly:
```sh
cd Part1 && make
cd Part2_Case1 && make
cd Part2_Case2 && make
cd Part2_Case2_Enhanced && make
```

---

## 4. Fault-Injection Mechanism

To rigorously test recovery, the Part 2 servers inject ungraceful connection drops:

1. **Per-Request Bernoulli Trial:** For each request, the server evaluates a Bernoulli trial with failure probability $p \in [0.0, 1.0]$.
2. **Reproducible SplitMix64 PRNG:** To ensure deterministic, statistically independent trials across threads, the random state is derived using a SplitMix64 avalanche hash seeded with `seed ^ connection_id` and advanced via POSIX `erand48()`.
3. **Uniform Cut Point $K$:** If the trial triggers failure, a cut point $K$ is drawn uniformly over the total bytes $R$ of the payload: $K \sim U[0, R-1]$.
4. **Hard TCP RST Abort:** When exactly $K$ bytes have been written to the socket, the server abruptly severs the connection using `SO_LINGER`:
   ```c
   struct linger sl = { .l_onoff = 1, .l_linger = 0 };
   setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
   close(fd); /* Emits an immediate TCP RST, purging unread socket buffers */
   ```

---

## 5. Variant Details & Execution Guide

### Part I: Baseline Single & Multi-threaded Server
- **Server:** Maps topics to directory paths using an in-memory index. In `mode=single`, clients are served sequentially in the main thread. In `mode=multi`, a detached POSIX worker thread is spawned per client connection.
  ```sh
  cd Part1
  ./server 8080 ../raw-img
  # Optional trailing flags: mode=single|multi max_clients=128 log=1 metrics=server_metrics.csv
  ```
- **Client:** Connects, sends `GET <topic>`, and writes each file to `<output_dir>/<topic>/<name>.part`, renaming it upon completion.
  ```sh
  ./client 127.0.0.1 8080 mucca /tmp/output
  ```

---

### Part II — Case 1: Naive Restart Retry Loop
- **Architecture:** The server maintains no session state. Upon disconnection, naive client retry logic discards any partially transferred files, applies exponential backoff ($100\text{ ms} \to 2\text{ s}$ ceiling), reconnects, and re-requests the entire topic from byte 0.
  ```sh
  cd Part2_Case1
  ./server 8081 ../raw-img 0.3
  # Client:
  ./client 127.0.0.1 8081 mucca /tmp/output
  ```
- **Testing & Verification:**
  ```sh
  ./test_case1.sh 8081 mucca 0.3
  ./validate_case1.sh
  ```

---

### Part II — Case 2: Session Management & Resumable Checkpoints
- **Architecture:** 
  - **Server:** Maintains a thread-safe 256-bucket hash table of active sessions keyed by 128-bit hex UUIDs (`sid`), tracking a monotonic high-water mark `hw_pos` per session.
  - **Client:** Checkpoints progress durably into `.ckpt` (writing to `.ckpt.tmp` and executing an atomic POSIX `rename()`).
  - **Resume Handshake:** On disconnect, the client reconnects and issues `RESUME <sid> <topic> <file_idx> <offset>`. The server validates that $\text{pos}(\text{idx}, \text{off}) \le \text{hw\_pos}$, accepts via `RESUMED`, and streams from that exact byte offset.
  ```sh
  cd Part2_Case2
  ./server 8082 ../raw-img 0.3
  # Client:
  ./client 127.0.0.1 8082 mucca /tmp/output
  ```
- **Testing & Verification:**
  ```sh
  ./test_case2.sh 8082 mucca 0.3
  ./validate_case2.sh
  ```

---

### Part II — Case 2 Enhanced: Parallel TCP Sockets & Work Queue
- **Architecture:**
  - **Manifest Exchange:** On connection, the server sends a complete topic manifest.
  - **Dynamic Task Queue:** The client partitions files into batches and queues them onto a synchronized work queue.
  - **Parallel Workers:** Spawns $N=4$ concurrent worker connections (`ATTACH <sid>`). Workers dequeue tasks, stream files via `FETCH`, and write to disk concurrently.
  - **Isolated Fault Recovery:** If worker connection $i$ suffers a TCP RST fault, only its uncompleted batch chunk is pushed to the head of the work queue. The remaining $N-1$ connections continue streaming at line rate.
  ```sh
  cd Part2_Case2_Enhanced
  ./server 8083 ../raw-img 0.3
  # Client:
  ./client 127.0.0.1 8083 mucca /tmp/output
  ```
- **Testing:**
  ```sh
  ./test_enhanced.sh 8083 mucca 0.3
  ```

---

## 6. End-to-End Integrity Verification & Fault Testing

### Automated Multi-Case Fault Recovery Test Suite
To automatically verify that all three Part II implementations survive non-zero network fault drops, correctly recover, and verify 100% SHA-256 byte parity, run:
```sh
make verify-faults
# Or directly:
./scripts/verify_fault_recovery.sh
```
This script tests Part 2 Case 1 (naive restart), Part 2 Case 2 (session checkpoint resume), and Part 2 Case 2 Enhanced (parallel TCP worker fault isolation) under active fault injection ($p=0.4$), verifying that:
- Injected TCP RST faults are intercepted and logged
- Interrupted transfers recover automatically
- Zero temporary artifacts (`*.part`, `*.ckpt*`, `*.tmp`) remain
- 100% of downloaded files match source SHA-256 digests

### Standalone File Integrity Verification
The test harness `scripts/verify.sh` validates full delivery correctness against any source dataset directory:
1. Checks that zero temporary artifacts (`*.part`, `*.ckpt*`, `*.tmp`) remain in `<output_dir>/<topic>/`.
2. Verifies that source and destination file counts match.
3. Computes cryptographic SHA-256 hashes of every file, asserting exact matches.

```sh
./scripts/verify.sh /path/to/dataset /tmp/output mucca
```

---

## 7. Performance Benchmarking & Result Regeneration

To rerun the full experiment suite and regenerate all throughput and overhead plots:
```sh
# Run Part 1 concurrency benchmarks (single-threaded vs multi-threaded, clients N=1..16)
./scripts/run_part1.sh

# Run Part 2 fault recovery sweeps across p in {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}
./scripts/run_part2.sh

# Regenerate visualization plots from CSV data into Results/
python3 scripts/plot.py
```
> [!NOTE]
> All throughput metrics are reported in **MB/s** (MegaBytes per second, $10^6$ bytes/sec) throughout datasets (`throughput_mb_s`), plots, and the project report.

All generated datasets and high-resolution plots are stored under `Results/`.
