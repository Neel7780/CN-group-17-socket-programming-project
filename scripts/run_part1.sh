#!/usr/bin/env bash
# ==============================================================================
# run_part1.sh — Automated Benchmark Harness for Part I
# Single-Threaded vs. Multi-Threaded Scalability Sweep
# IT305 Computer Networks Lab · Group 17 (021 & 034)
# Author: Manal Patel (Roll No. 202401112)
#
# Sweeps concurrent client load N in {1, 2, 4, 8, 16} across single and multi-threaded
# server modes. Captures per-client transfer duration, queueing delay, and aggregate
# system throughput (MB/s).
# ==============================================================================
set -u

TOPICS_ROOT="${1:-raw-img}"
TOPIC="${2:-mucca}"
PORT_BASE="${3:-9200}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PART1_DIR="$REPO_ROOT/Part1"
RESULTS_DIR="$REPO_ROOT/Results"
OUT_BASE="/tmp/cn_part1_benchmark"

mkdir -p "$RESULTS_DIR" "$OUT_BASE"

echo "=========================================================="
echo " Starting Part I Throughput & Concurrency Benchmark"
echo " Dataset Root: $TOPICS_ROOT | Topic: $TOPIC"
echo " Results Dir:  $RESULTS_DIR"
echo "=========================================================="

# Build client if Makefile exists
if [ -f "$PART1_DIR/Makefile" ]; then
    echo "[*] Building Part 1 binaries..."
    (cd "$PART1_DIR" && make all >/dev/null 2>&1 || true)
fi

CLIENT_BIN="$PART1_DIR/client"
if [ ! -x "$CLIENT_BIN" ]; then
    echo "[-] Error: Part 1 client binary not found at $CLIENT_BIN"
    exit 1
fi

# Detect server binary (Part1/server or fallback to Part2 server with p=0)
if [ -x "$PART1_DIR/server" ]; then
    SERVER_BIN="$PART1_DIR/server"
    SERVER_IS_PART1=1
elif [ -x "$REPO_ROOT/Part2_Case2/server" ]; then
    SERVER_BIN="$REPO_ROOT/Part2_Case2/server"
    SERVER_IS_PART1=0
    echo "[*] Note: Using Part2_Case2 server with p=0.0 for Part I emulation."
else
    echo "[-] Warning: No server binary found in Part1 or Part2_Case2. Please build server first."
    SERVER_BIN=""
fi

PORT=$PORT_BASE

for MODE in single multi; do
    CSV_OUT="$RESULTS_DIR/Part1_${MODE^}Thread_throughput.csv"
    echo "[*] Initializing $CSV_OUT"
    echo "n_clients,wall_start_ns,wall_end_ns,role,mode,conn_id,topic,files,payload_bytes,wire_bytes,wait_ms,duration_ms,throughput_mbps,status" > "$CSV_OUT"

    for N in 1 2 4 8 16; do
        echo "----------------------------------------------------------"
        echo " Benchmark: Mode=$MODE | Concurrent Clients N=$N | Port=$PORT"
        echo "----------------------------------------------------------"

        RUN_DIR="$OUT_BASE/${MODE}_N${N}"
        rm -rf "$RUN_DIR" && mkdir -p "$RUN_DIR"

        if [ -n "$SERVER_BIN" ]; then
            # Launch server in background
            if [ "$SERVER_IS_PART1" -eq 1 ]; then
                "$SERVER_BIN" "$PORT" "$TOPICS_ROOT" "mode=$MODE" "log=0" &
            else
                "$SERVER_BIN" "$PORT" "$TOPICS_ROOT" 0.0 "log=0" &
            fi
            SRV_PID=$!
            sleep 0.8
        fi

        PIDS=()
        START_TIME_MS=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')

        for i in $(seq 1 $N); do
            CLI_DIR="$RUN_DIR/cli_$i"
            mkdir -p "$CLI_DIR"
            METRICS_FILE="$RUN_DIR/metrics_$i.csv"

            # Part1 client supports metrics=PATH and tag=LABEL
            "$CLIENT_BIN" 127.0.0.1 "$PORT" "$TOPIC" "$CLI_DIR" \
                log=0 metrics="$METRICS_FILE" tag="${MODE}_n${N}" >/dev/null 2>&1 &
            PIDS+=($!)
        done

        # Wait for all client processes to complete
        for pid in "${PIDS[@]}"; do
            wait "$pid" 2>/dev/null || true
        done

        END_TIME_MS=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')
        WALL_DUR_MS=$((END_TIME_MS - START_TIME_MS))
        [ "$WALL_DUR_MS" -le 0 ] && WALL_DUR_MS=1

        # Stop server
        if [ -n "$SERVER_BIN" ]; then
            kill -TERM "$SRV_PID" 2>/dev/null || true
            wait "$SRV_PID" 2>/dev/null || true
        fi

        # Consolidate metrics rows into CSV_OUT
        ROWS_ADDED=0
        for i in $(seq 1 $N); do
            METRICS_FILE="$RUN_DIR/metrics_$i.csv"
            if [ -f "$METRICS_FILE" ]; then
                tail -n +2 "$METRICS_FILE" | while IFS= read -r line; do
                    [ -n "$line" ] && echo "$N,$line" >> "$CSV_OUT"
                    ROWS_ADDED=$((ROWS_ADDED + 1))
                done
            fi
        done

        echo "[+] Completed N=$N in ${WALL_DUR_MS}ms (Aggregated metrics rows: $ROWS_ADDED)"
        PORT=$((PORT + 1))
        sleep 0.5
    done
done

echo ""
echo "[+] Part I Benchmark successfully concluded. CSV results saved in $RESULTS_DIR"
