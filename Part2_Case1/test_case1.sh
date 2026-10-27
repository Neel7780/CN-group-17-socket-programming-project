#!/usr/bin/env bash
#
# Part II Case 1 test harness.
#
#   ./test_case1.sh [port] [topic] [failure_probability]
#
# Verifies that the transfer survives the injected faults, that every file is
# byte-for-byte identical to the source dataset, and that no partial file is
# left behind.
#
set -u

PORT=${1:-9201}
TOPIC=${2:-mucca}
P=${3:-0.3}
DATASET_DIR=${DATASET_DIR:-../raw-img}
OUT_DIR=${OUT_DIR:-/tmp/case1_test_${TOPIC}_$$}

echo "=========================================================="
echo " Part II Case 1 (Fault Injection + Restart From Zero)"
echo " Topic: $TOPIC | Port: $PORT | p: $P"
echo "=========================================================="

make clean >/dev/null 2>&1
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

./server "$PORT" "$DATASET_DIR" "$P" log=2 > /tmp/case1_server_$$.log 2>&1 &
SERVER_PID=$!
sleep 1

cleanup() {
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f /tmp/case1_server_$$.log
}
trap cleanup EXIT

START_TIME=$(date +%s%N)
./client 127.0.0.1 "$PORT" "$TOPIC" "$OUT_DIR" log=2 metrics="$OUT_DIR/client_metrics.csv"
CLIENT_RC=$?
END_TIME=$(date +%s%N)

ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))
echo "Client exit code: $CLIENT_RC"
echo "Transfer completed in $ELAPSED_MS ms."

FAULTS=$(grep -c "^.*FAULT conn=" /tmp/case1_server_$$.log 2>/dev/null || echo 0)
echo "Injected faults observed by the server: $FAULTS"

if [ "$CLIENT_RC" -ne 0 ]; then
    echo "[-] Client failed"
    exit 1
fi

SRC_COUNT=$(find "$DATASET_DIR/$TOPIC" -type f | wc -l | tr -d ' ')
DST_COUNT=$(find "$OUT_DIR/$TOPIC" -type f | wc -l | tr -d ' ')

echo "Files in source dataset: $SRC_COUNT"
echo "Files downloaded:        $DST_COUNT"

if [ "$SRC_COUNT" -ne "$DST_COUNT" ]; then
    echo "[-] File count mismatch!"
    exit 1
fi

# A leftover .part file would mean an attempt was not cleaned up.
LEFTOVER=$(find "$OUT_DIR/$TOPIC" -name '*.part' | wc -l | tr -d ' ')
if [ "$LEFTOVER" -ne 0 ]; then
    echo "[-] Leftover partial files: $LEFTOVER"
    exit 1
fi

echo "Verifying SHA-256 checksums across downloaded files..."
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$DATASET_DIR/$TOPIC" && find . -type f -exec sha256sum {} + | sort ) > /tmp/case1_expected_$$.txt
    ( cd "$OUT_DIR/$TOPIC" && find . -type f -exec sha256sum {} + | sort ) > /tmp/case1_actual_$$.txt
else
    ( cd "$DATASET_DIR/$TOPIC" && find . -type f -exec shasum -a 256 {} + | sort ) > /tmp/case1_expected_$$.txt
    ( cd "$OUT_DIR/$TOPIC" && find . -type f -exec shasum -a 256 {} + | sort ) > /tmp/case1_actual_$$.txt
fi

if diff -q /tmp/case1_expected_$$.txt /tmp/case1_actual_$$.txt >/dev/null; then
    echo "[+] SUCCESS: All $SRC_COUNT files verified with 100% integrity (0 mismatches)!"
    rm -f /tmp/case1_expected_$$.txt /tmp/case1_actual_$$.txt
    exit 0
else
    echo "[-] Checksum verification FAILED"
    diff /tmp/case1_expected_$$.txt /tmp/case1_actual_$$.txt | head -20
    rm -f /tmp/case1_expected_$$.txt /tmp/case1_actual_$$.txt
    exit 1
fi
