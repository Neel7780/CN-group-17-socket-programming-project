# IT305: Computer Networks Lab — Final Project Report
## Fault Tolerant Topic-based File Distribution Service

**Dhirubhai Ambani University (DAU)**  
**Department of Information and Communication Technology**  
**Autumn Semester 2026–2027**  

---

### Project Metadata
* **Project Group:** 17
* **Merged Lab Groups:** 021 and 034
* **Platform:** C11 on Linux, POSIX Sockets (IPv4 TCP), POSIX Threads, GNU Make

---

### Table of Contributors & Individual Responsibilities

| Sr. | Student Name | Roll No. | Group | Individual Planned & Delivered Contribution |
|---|---|---|---|---|
| 1 | **Aaditya Kaushik** | 202401001 | Group 034 | Part I server: topic-directory map, single-threaded and multi-threaded request dispatch |
| 2 | **Nisarg Trivedi** | 202401129 | Group 034 | Part I client: buffered line reader, atomic file reassembly, synchronous metrics logger |
| 3 | **Niranjan Dangi** | 202401042 | Group 034 | Fault-injection subsystem (`SO_LINGER` TCP RST) and Part II Case 1 exponential retry loop |
| 4 | **Kavish Inani** | 202401089 | Group 021 | Part II Case 2: 256-bucket session table, high-water mark validation, and resume protocol |
| 5 | **Neel Khatri** | 202401093 | Group 021 | Part II Case 2 Enhanced: dynamic client work queue, $N=4$ parallel TCP workers, `sendfile` |
| 6 | **Manal Patel** | 202401112 | Group 021 | **Automated benchmark harnesses (`run_part1.sh`, `run_part2.sh`), SHA-256 verifier (`verify.sh`), plot engine (`plot.py`), and final report integration** |

---

## 1. System Overview & Architecture

The service distributes categorized multi-file topic directories from a server to clients over IPv4 TCP. Control framing uses single ASCII text lines terminated by `\n`, followed immediately by raw binary file payloads. This provides netcat-compatible human observability with negligible overhead (<0.1% framing overhead).

### 1.1 Wire Protocol Across Variants

| Variant | Client Request | Server Response | Transfer Semantics & State |
|---|---|---|---|
| **Part I** | `GET <topic>` | `OK <count> <bytes>` | Sequential `FILE <size> <name>` + payload, terminating with `END`. |
| **Case 1** | `GET <topic>` | `OK <count> <bytes>` | Single Bernoulli fault cut at byte $K$. Client discards state and restarts from byte 0. |
| **Case 2** | `GET <topic>`<br/>`RESUME <sid> <topic> <idx> <off>` | `SESSION <sid> <cnt> <bytes>`<br/>`RESUMED <idx> <off> <rem>` | Client flushes atomic `.ckpt`; Server verifies $pos(idx, off) \le hw\_pos$ and resumes from delta. |
| **Enhanced**| `FETCH <sid> <start> <off> <end>` | `BATCH <files> <bytes>` | $N=4$ simultaneous TCP sockets dynamically consume ~1MB batches from thread-safe work queue. |

---

## 2. Part I Performance Analysis: Single vs. Multi-Threaded Scalability

We swept concurrent client load $N \in \{1, 2, 4, 8, 16\}$ downloading the reference `mucca` topic (1,860 images, 25.41 MB).

### 2.1 Single-Threaded Bottleneck
* **Aggregate Throughput:** Plateaus at **91.5 MB/s** across all $N$.
* **Queueing Delay:** The single thread accepts and transfers clients strictly one at a time. Pending clients wait in the socket backlog buffer (128). Average wait time accumulates linearly up to **4,125 ms** at $N=16$.
* **Per-Client Goodput:** Scales down as $T_{client} \approx \frac{91.5}{N}\text{ MB/s}$.

### 2.2 Multi-Threaded Concurrent Scaling
* **Aggregate Throughput:** Spawning detached worker threads per client overlaps network I/O, scaling aggregate throughput:
  * $N=1$: 91.5 MB/s
  * $N=2$: 174.0 MB/s
  * $N=4$: 312.5 MB/s
  * $N=8$: 458.0 MB/s
  * $N=16$: **524.0 MB/s** (saturates loopback / page cache memory bandwidth).
* **Per-Client Latency:** Wait time remains $<15\text{ ms}$ regardless of concurrency level.

---

## 3. Part II Performance Analysis: Fault Recovery Strategies

Fault injection uses `SO_LINGER (l_linger=0)` to emit ungraceful TCP RST aborts at a cut point $K \sim U[0, R-1]$ evaluated once per request with probability $p \in \{0.0, 0.2, 0.5, 0.8\}$.

### 3.1 Case 1: Naive Full Restart
* Because each attempt is an independent Bernoulli trial, the expected attempts until completion follows a geometric distribution:
  $$E[\text{Attempts}] = \frac{1}{1 - p}$$
* At $p=0.8$, transfers require an average of **5 attempts**.
* Retransmission overhead reaches **~3.0×** (200% redundant bytes retransmitted).
* Effective throughput collapses from **87.6 MB/s** ($p=0$) down to **29.2 MB/s** ($p=0.8$).

### 3.2 Case 2: Atomic Checkpoint & Session Resumption
* The client flushes durable state (`.ckpt.tmp` $\to$ `fsync()` $\to$ `rename()`) after every file and every 64 KiB within large files.
* Upon reconnection, the server validates the checkpoint against `hw_pos` and resumes streaming from the exact byte offset.
* **Overhead Factor:** Remains between **1.001× and 1.010×** across all fault rates.
* **Effective Goodput:** Sustains **72.5 MB/s** at $p=0.8$—over **2.5× faster** than Case 1.

---

## 4. Part II Case 2 Enhanced & Multi-Case Synthesis

Case 2 Enhanced deploys $N=4$ simultaneous TCP sockets with dynamic work queueing and zero-copy `sendfile()`.

### 4.1 Consolidated Benchmark Results

| Evaluation Case | Throughput ($p=0.0$) | Throughput ($p=0.2$) | Throughput ($p=0.5$) | Throughput ($p=0.8$) | Wire Overhead Factor ($p=0.8$) |
|---|---|---|---|---|---|
| **Part I (Single Client)** | 91.5 MB/s | N/A | N/A | N/A | 1.00× (0% redundant) |
| **Case 1 (Naive Restart)** | 87.6 MB/s | 78.1 MB/s | 58.4 MB/s | 29.2 MB/s | 3.00× (200% redundant) |
| **Case 2 (Session Resume)** | 86.1 MB/s | 84.3 MB/s | 81.2 MB/s | 72.5 MB/s | 1.01× (<1% redundant) |
| **Case 2 Enhanced (4× TCP)** | **310.0 MB/s** | **285.4 MB/s** | **242.0 MB/s** | **185.3 MB/s** | **1.02× (~1.5% redundant)** |

### 4.2 Key Architectural Takeaways
1. **Parallel Multiplexing:** Achieving **310 MB/s** at $p=0$ represents a **3.6× speedup** over single-socket transfers.
2. **Fault Isolation:** A severed socket only aborts its active 1MB batch, which is re-enqueued at the head of the queue. The remaining 3 connections continue streaming uninterrupted. At $p=0.8$, Enhanced delivers **185.3 MB/s**—over **6.3× faster than Case 1**.
3. **Data Integrity:** `scripts/verify.sh` confirmed 100% cryptographic SHA-256 integrity on all 1,860 files with zero leftover `.part` or `.ckpt` files.

---

## 5. Repository & Submission Structure

```
FinalSubmission_Group_021_034/
├── FinalReport_Group_021_034.pdf
├── FinalReport_Group_021_034.md
├── README.md
├── Part1/
├── Part2_Case1/
├── Part2_Case2/
├── Part2_Case2_Enhanced/
├── Results/
│   ├── Part1_SingleThread_throughput.[csv|png]
│   ├── Part1_MultiThread_throughput.[csv|png]
│   ├── Part2_Case1_throughput.[csv|png]
│   ├── Part2_Case1_overhead.png
│   ├── Part2_Case2_throughput.[csv|png]
│   ├── Part2_Case2_overhead.png
│   ├── Part2_Case2Enhanced_throughput.[csv|png]
│   ├── Part2_Case2Enhanced_overhead.png
│   └── Part2_Comparison_throughput.png
└── scripts/
    ├── run_part1.sh
    ├── run_part2.sh
    ├── verify.sh
    └── plot.py
```
