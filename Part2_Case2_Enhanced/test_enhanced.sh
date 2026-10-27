#!/usr/bin/env bash
set -e

PORT=${1:-9123}
TOPIC=${2:-scoiattolo}
P=${3:-0.0}
CONNS=${4:-4}
DATASET_DIR="../raw-img"
OUT_DIR="/tmp/test_download_${TOPIC}"

echo "=========================================================="
echo " Testing Part II Case 2 Enhanced (Parallel TCP + Fault)"
echo " Topic: $TOPIC | Port: $PORT | p: $P | Conns: $CONNS"
echo "=========================================================="

# Build binaries
make clean
make

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# Launch Server in background
./server "$PORT" "$DATASET_DIR" "$P" log=1 &
SERVER_PID=$!
sleep 1

cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Run Client
echo "Starting client..."
START_TIME=$(date +%s%N)
./client 127.0.0.1 "$PORT" "$TOPIC" "$OUT_DIR" conns="$CONNS" log=1
END_TIME=$(date +%s%N)

ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))
echo "Transfer completed in $ELAPSED_MS ms."

# Verification
SRC_COUNT=$(ls -1 "$DATASET_DIR/$TOPIC" | wc -l)
DST_COUNT=$(ls -1 "$OUT_DIR/$TOPIC" | wc -l)

echo "Files in source dataset: $SRC_COUNT"
echo "Files downloaded:        $DST_COUNT"

if [ "$SRC_COUNT" -ne "$DST_COUNT" ]; then
    echo "[-] File count mismatch!"
    exit 1
fi

echo "Verifying SHA-256 checksums across downloaded files..."
cd "$DATASET_DIR/$TOPIC"
sha256sum * > /tmp/expected_checksums.txt
cd "$OUT_DIR/$TOPIC"
sha256sum -c /tmp/expected_checksums.txt > /tmp/checksum_results.txt 2>&1
FAILED=$(grep -v "OK$" /tmp/checksum_results.txt | wc -l)

if [ "$FAILED" -eq 0 ]; then
    echo "[+] SUCCESS: All $SRC_COUNT files verified with 100% integrity (0 mismatches)!"
else
    echo "[-] Checksum verification failed on $FAILED files."
    exit 1
fi
