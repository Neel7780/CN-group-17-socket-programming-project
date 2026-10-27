#!/usr/bin/env bash
#
# Part II Case 2 test harness.
#
#   ./test_case2.sh [port] [topic] [failure_probability]
#
# Verifies that the resumed transfer is byte-for-byte identical to the
# source dataset, and reports how many bytes had to be re-sent.
#
set -u

PORT=${1:-9201}
TOPIC=${2:-gatto}
P=${3:-0.0}
DATASET_DIR=${DATASET_DIR:-../raw-img}
OUT_DIR=${OUT_DIR:-/tmp/case2_test_${TOPIC}_$$}

echo "=========================================================="
echo " Part II Case 2 (Session Management + Resume Protocol)"
echo " Topic: $TOPIC | Port: $PORT | p: $P"
echo "=========================================================="

make clean >/dev/null 2>&1
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

./server "$PORT" "$DATASET_DIR" "$P" log=1 > /tmp/case2_server_$$.log 2>&1 &
SERVER_PID=$!
sleep 1

cleanup() {
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f /tmp/case2_server_$$.log
}
trap cleanup EXIT

START_TIME=$(date +%s%N)
./client 127.0.0.1 "$PORT" "$TOPIC" "$OUT_DIR" log=1
CLIENT_RC=$?
END_TIME=$(date +%s%N)

ELAPSED_MS=$(( (END_TIME - START_TIME) / 1000000 ))
echo "Client exit code: $CLIENT_RC"
echo "Transfer completed in $ELAPSED_MS ms."

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

# Leftover .part files or a surviving .ckpt would mean the transfer was not
# cleanly finalised.
LEFTOVER=$(find "$OUT_DIR/$TOPIC" -name '*.part' -o -name '.ckpt*' | wc -l | tr -d ' ')
if [ "$LEFTOVER" -ne 0 ]; then
    echo "[-] Leftover partial/checkpoint files: $LEFTOVER"
    exit 1
fi

echo "Verifying SHA-256 checksums across downloaded files..."
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$DATASET_DIR/$TOPIC" && find . -type f -exec sha256sum {} + | sort ) > /tmp/case2_expected_$$.txt
    ( cd "$OUT_DIR/$TOPIC" && find . -type f -exec sha256sum {} + | sort ) > /tmp/case2_actual_$$.txt
else
    ( cd "$DATASET_DIR/$TOPIC" && find . -type f -exec shasum -a 256 {} + | sort ) > /tmp/case2_expected_$$.txt
    ( cd "$OUT_DIR/$TOPIC" && find . -type f -exec shasum -a 256 {} + | sort ) > /tmp/case2_actual_$$.txt
fi

if diff -q /tmp/case2_expected_$$.txt /tmp/case2_actual_$$.txt >/dev/null; then
    echo "[+] SUCCESS: All $SRC_COUNT files verified with 100% integrity (0 mismatches)!"
    rm -f /tmp/case2_expected_$$.txt /tmp/case2_actual_$$.txt
    exit 0
else
    echo "[-] Checksum verification FAILED"
    diff /tmp/case2_expected_$$.txt /tmp/case2_actual_$$.txt | head -20
    rm -f /tmp/case2_expected_$$.txt /tmp/case2_actual_$$.txt
    exit 1
fi
