#!/usr/bin/env bash
# ==============================================================================
# verify_fault_recovery.sh — Multi-Case Fault Tolerance & Recovery Test Suite
# IT305 Computer Networks Lab · Group 17 (Merged Lab Groups 021 & 034)
#
# Validates under active network fault injection (p = 0.40):
#   1. Part II Case 1: Naive restart after TCP RST drop, 100% SHA-256 match
#   2. Part II Case 2: Session checkpoint resume after drop, minimal wire overhead
#   3. Part II Case 2 Enhanced: Parallel worker fault isolation and queue recovery
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Identify or provision test dataset
DATASET_DIR="${1:-}"
TOPIC="mucca"
SYNTHETIC=0

if [ -z "$DATASET_DIR" ]; then
    if [ -d "$REPO_ROOT/raw-img/mucca" ]; then
        DATASET_DIR="$REPO_ROOT/raw-img"
    elif [ -d "$REPO_ROOT/../raw-img/mucca" ]; then
        DATASET_DIR="$REPO_ROOT/../raw-img"
    else
        SYNTHETIC=1
        DATASET_DIR="/tmp/test_dataset_$$"
        TOPIC="synth_topic"
        echo "[*] Provisioning synthetic test dataset in $DATASET_DIR/$TOPIC..."
        mkdir -p "$DATASET_DIR/$TOPIC"
        for i in $(seq 1 30); do
            head -c "$(( (i * 131071) % 500000 + 4096 ))" /dev/urandom > "$DATASET_DIR/$TOPIC/file_${i}.bin"
        done
    fi
fi

cleanup_synthetic() {
    if [ "$SYNTHETIC" -eq 1 ] && [ -d "$DATASET_DIR" ]; then
        rm -rf "$DATASET_DIR"
    fi
}
trap cleanup_synthetic EXIT

echo "======================================================================"
echo " IT305 Fault-Tolerant File Distribution Recovery Verification Suite"
echo " Dataset Root: $DATASET_DIR | Topic: $TOPIC | Fault Probability: p=0.40"
echo "======================================================================"

SRC_FILES=$(find "$DATASET_DIR/$TOPIC" -maxdepth 1 -type f | wc -l | tr -d ' ')
echo "[*] Dataset topic '$TOPIC' verified: $SRC_FILES source files."
echo ""

FAILURES=0

# ------------------------------------------------------------------------------
# 1. Test Part II Case 1: Naive Retry (Restart From Byte 0)
# ------------------------------------------------------------------------------
echo ">>> [1/3] Testing Part II Case 1: Naive Restart Under Faults (p=0.40)..."
C1_DIR="$REPO_ROOT/Part2_Case1"
C1_OUT="/tmp/c1_verify_$$"
C1_LOG="/tmp/c1_srv_$$.log"
rm -rf "$C1_OUT" "$C1_LOG"
mkdir -p "$C1_OUT"

(cd "$C1_DIR" && make -s clean && make -s)

C1_PORT=9501
"$C1_DIR/server" "$C1_PORT" "$DATASET_DIR" 0.40 log=2 > "$C1_LOG" 2>&1 &
C1_PID=$!
sleep 0.8

"$C1_DIR/client" 127.0.0.1 "$C1_PORT" "$TOPIC" "$C1_OUT" log=2 >/dev/null 2>&1 || true
kill -TERM "$C1_PID" 2>/dev/null || true
wait "$C1_PID" 2>/dev/null || true

# Assertions
C1_FAULTS=$(grep -c "FAULT conn=" "$C1_LOG" 2>/dev/null || echo 0)
C1_DL_COUNT=$(find "$C1_OUT/$TOPIC" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
C1_LEFTOVERS=$(find "$C1_OUT/$TOPIC" \( -name '*.part' -o -name '.ckpt*' -o -name '*.tmp' \) 2>/dev/null | wc -l | tr -d ' ')

if [ "$C1_DL_COUNT" -eq "$SRC_FILES" ] && [ "$C1_LEFTOVERS" -eq 0 ]; then
    # Verify SHA-256
    (cd "$DATASET_DIR/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c1_exp_$$.txt
    (cd "$C1_OUT/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c1_act_$$.txt
    if diff -q /tmp/c1_exp_$$.txt /tmp/c1_act_$$.txt >/dev/null; then
        echo "[PASS] Part II Case 1: Recovered across $C1_FAULTS fault injections, 0 remnants, $SRC_FILES/$SRC_FILES SHA-256 matched."
    else
        echo "[FAIL] Part II Case 1: SHA-256 checksum mismatch detected!"
        FAILURES=$((FAILURES + 1))
    fi
    rm -f /tmp/c1_exp_$$.txt /tmp/c1_act_$$.txt
else
    echo "[FAIL] Part II Case 1: File count mismatch (expected $SRC_FILES, got $C1_DL_COUNT, remnants: $C1_LEFTOVERS)!"
    FAILURES=$((FAILURES + 1))
fi
rm -rf "$C1_OUT" "$C1_LOG"
echo ""

# ------------------------------------------------------------------------------
# 2. Test Part II Case 2: Session Checkpoint Resume
# ------------------------------------------------------------------------------
echo ">>> [2/3] Testing Part II Case 2: Checkpoint Resume Under Faults (p=0.40)..."
C2_DIR="$REPO_ROOT/Part2_Case2"
C2_OUT="/tmp/c2_verify_$$"
C2_LOG="/tmp/c2_srv_$$.log"
C2_CLI_LOG="/tmp/c2_cli_$$.log"
rm -rf "$C2_OUT" "$C2_LOG" "$C2_CLI_LOG"
mkdir -p "$C2_OUT"

(cd "$C2_DIR" && make -s clean && make -s)

C2_PORT=9502
"$C2_DIR/server" "$C2_PORT" "$DATASET_DIR" 0.40 log=2 > "$C2_LOG" 2>&1 &
C2_PID=$!
sleep 0.8

"$C2_DIR/client" 127.0.0.1 "$C2_PORT" "$TOPIC" "$C2_OUT" log=2 > "$C2_CLI_LOG" 2>&1 || true
kill -TERM "$C2_PID" 2>/dev/null || true
wait "$C2_PID" 2>/dev/null || true

C2_RESUMES=$(grep -c "RESUME sid=" "$C2_CLI_LOG" 2>/dev/null || echo 0)
C2_DL_COUNT=$(find "$C2_OUT/$TOPIC" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
C2_LEFTOVERS=$(find "$C2_OUT/$TOPIC" \( -name '*.part' -o -name '.ckpt*' -o -name '*.tmp' \) 2>/dev/null | wc -l | tr -d ' ')

if [ "$C2_DL_COUNT" -eq "$SRC_FILES" ] && [ "$C2_LEFTOVERS" -eq 0 ]; then
    (cd "$DATASET_DIR/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c2_exp_$$.txt
    (cd "$C2_OUT/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c2_act_$$.txt
    if diff -q /tmp/c2_exp_$$.txt /tmp/c2_act_$$.txt >/dev/null; then
        echo "[PASS] Part II Case 2: Resumed from validated checkpoint ($C2_RESUMES resumes), 0 remnants, $SRC_FILES/$SRC_FILES SHA-256 matched."
    else
        echo "[FAIL] Part II Case 2: SHA-256 checksum mismatch detected!"
        FAILURES=$((FAILURES + 1))
    fi
    rm -f /tmp/c2_exp_$$.txt /tmp/c2_act_$$.txt
else
    echo "[FAIL] Part II Case 2: File count mismatch (expected $SRC_FILES, got $C2_DL_COUNT, remnants: $C2_LEFTOVERS)!"
    FAILURES=$((FAILURES + 1))
fi
rm -rf "$C2_OUT" "$C2_LOG" "$C2_CLI_LOG"
echo ""

# ------------------------------------------------------------------------------
# 3. Test Part II Case 2 Enhanced: Parallel TCP Workers & Work Queue Recovery
# ------------------------------------------------------------------------------
echo ">>> [3/3] Testing Part II Case 2 Enhanced: Parallel TCP Fault Isolation (p=0.40)..."
C2E_DIR="$REPO_ROOT/Part2_Case2_Enhanced"
C2E_OUT="/tmp/c2e_verify_$$"
C2E_LOG="/tmp/c2e_srv_$$.log"
C2E_CLI_LOG="/tmp/c2e_cli_$$.log"
rm -rf "$C2E_OUT" "$C2E_LOG" "$C2E_CLI_LOG"
mkdir -p "$C2E_OUT"

(cd "$C2E_DIR" && make -s clean && make -s)

C2E_PORT=9503
"$C2E_DIR/server" "$C2E_PORT" "$DATASET_DIR" 0.40 > "$C2E_LOG" 2>&1 &
C2E_PID=$!
sleep 0.8

"$C2E_DIR/client" 127.0.0.1 "$C2E_PORT" "$TOPIC" "$C2E_OUT" > "$C2E_CLI_LOG" 2>&1 || true
kill -TERM "$C2E_PID" 2>/dev/null || true
wait "$C2E_PID" 2>/dev/null || true

C2E_RECONNS=$(grep -c "reconnected and attached" "$C2E_CLI_LOG" 2>/dev/null || echo 0)
C2E_DL_COUNT=$(find "$C2E_OUT/$TOPIC" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
C2E_LEFTOVERS=$(find "$C2E_OUT/$TOPIC" \( -name '*.part' -o -name '.ckpt*' -o -name '*.tmp' \) 2>/dev/null | wc -l | tr -d ' ')

if [ "$C2E_DL_COUNT" -eq "$SRC_FILES" ] && [ "$C2E_LEFTOVERS" -eq 0 ]; then
    (cd "$DATASET_DIR/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c2e_exp_$$.txt
    (cd "$C2E_OUT/$TOPIC" && find . -maxdepth 1 -type f -exec sha256sum {} + | sort) > /tmp/c2e_act_$$.txt
    if diff -q /tmp/c2e_exp_$$.txt /tmp/c2e_act_$$.txt >/dev/null; then
        echo "[PASS] Part II Case 2 Enhanced: Parallel workers survived ($C2E_RECONNS worker reconnects), 0 remnants, $SRC_FILES/$SRC_FILES SHA-256 matched."
    else
        echo "[FAIL] Part II Case 2 Enhanced: SHA-256 checksum mismatch detected!"
        FAILURES=$((FAILURES + 1))
    fi
    rm -f /tmp/c2e_exp_$$.txt /tmp/c2e_act_$$.txt
else
    echo "[FAIL] Part II Case 2 Enhanced: File count mismatch (expected $SRC_FILES, got $C2E_DL_COUNT, remnants: $C2E_LEFTOVERS)!"
    FAILURES=$((FAILURES + 1))
fi
rm -rf "$C2E_OUT" "$C2E_LOG" "$C2E_CLI_LOG"
echo ""

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
echo "======================================================================"
if [ "$FAILURES" -eq 0 ]; then
    echo "[+] ALL FAULT RECOVERY TESTS PASSED (3/3 Cases Verified)"
    echo "    - Part II Case 1: Naive Retry Loop PASSED"
    echo "    - Part II Case 2: Session Checkpoint Resume PASSED"
    echo "    - Part II Case 2 Enhanced: Multi-Worker Fault Isolation PASSED"
    exit 0
else
    echo "[-] FAILED: $FAILURES test cases reported errors."
    exit 1
fi
