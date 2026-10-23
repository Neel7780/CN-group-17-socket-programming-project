#!/usr/bin/env bash
# ==============================================================================
# verify.sh — End-to-End File Integrity & Remnant Verifier
# IT305 Computer Networks Lab · Group 17 (021 & 034)
# Author: Manal Patel (Roll No. 202401112)
#
# Verifies that:
#   1. Every file in <server_topics_root>/<topic> exists in <output_dir>/<topic>
#   2. No temporary file remnants (*.part, *.ckpt, *.tmp) linger in output dir
#   3. SHA-256 cryptographic hashes match byte-for-byte with source
# ==============================================================================
set -u

if [ $# -lt 3 ]; then
    echo "Usage: $0 <server_topics_root_dir> <output_dir> <topic>"
    exit 1
fi

SERVER_ROOT="$1"
OUTPUT_DIR="$2"
TOPIC="$3"

SRC_DIR="$SERVER_ROOT/$TOPIC"
DST_DIR="$OUTPUT_DIR/$TOPIC"

if [ ! -d "$SRC_DIR" ]; then
    echo "[-] Error: Source topic directory does not exist: $SRC_DIR"
    exit 1
fi

if [ ! -d "$DST_DIR" ]; then
    echo "[-] Error: Destination topic directory does not exist: $DST_DIR"
    exit 1
fi

echo "=========================================================="
echo " Verifying Topic: $TOPIC"
echo " Source: $SRC_DIR"
echo " Target: $DST_DIR"
echo "=========================================================="

# 1. Check for temporary / leftover files (.part, .ckpt*, .tmp)
LEFTOVERS=$(find "$DST_DIR" \( -name '*.part' -o -name '.ckpt*' -o -name '*.tmp' \) 2>/dev/null | wc -l | tr -d ' ')
if [ "$LEFTOVERS" -ne 0 ]; then
    echo "[-] FAILED: Found $LEFTOVERS temporary leftover files in destination directory!"
    find "$DST_DIR" \( -name '*.part' -o -name '.ckpt*' -o -name '*.tmp' \)
    exit 1
else
    echo "[+] Clean state verified: 0 temporary leftovers (*.part, .ckpt*)"
fi

# 2. Check file counts
SRC_COUNT=$(find "$SRC_DIR" -maxdepth 1 -type f | wc -l | tr -d ' ')
DST_COUNT=$(find "$DST_DIR" -maxdepth 1 -type f | wc -l | tr -d ' ')

echo "[*] Source file count: $SRC_COUNT"
echo "[*] Target file count: $DST_COUNT"

if [ "$SRC_COUNT" -ne "$DST_COUNT" ]; then
    echo "[-] FAILED: File count mismatch! Source has $SRC_COUNT, Destination has $DST_COUNT."
    exit 1
fi

# 3. Check SHA-256 for all files
PASS_COUNT=0
FAIL_COUNT=0

# Detect available sha256 hashing utility
if command -v sha256sum >/dev/null 2>&1; then
    HASH_CMD="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    HASH_CMD="shasum -a 256"
else
    echo "[-] Error: Neither sha256sum nor shasum is available on PATH."
    exit 1
fi

echo "[*] Computing cryptographic digests using '$HASH_CMD'..."
for src_file in "$SRC_DIR"/*; do
    [ -f "$src_file" ] || continue
    fname=$(basename "$src_file")
    dst_file="$DST_DIR/$fname"

    if [ ! -f "$dst_file" ]; then
        echo "[-] MISSING: $fname"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue
    fi

    src_hash=$($HASH_CMD "$src_file" | awk '{print $1}')
    dst_hash=$($HASH_CMD "$dst_file" | awk '{print $1}')

    if [ "$src_hash" = "$dst_hash" ]; then
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "[-] HASH MISMATCH: $fname (Expected: $src_hash, Received: $dst_hash)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo "----------------------------------------------------------"
echo " Verification Summary:"
echo "   Total Checked: $SRC_COUNT"
echo "   Passed:        $PASS_COUNT"
echo "   Failed:        $FAIL_COUNT"
echo "----------------------------------------------------------"

if [ "$FAIL_COUNT" -eq 0 ] && [ "$SRC_COUNT" -gt 0 ]; then
    echo "[+] SUCCESS: 100% integrity verified! All files match byte-for-byte."
    exit 0
else
    echo "[-] FAILED: Integrity verification failed with $FAIL_COUNT errors."
    exit 1
fi
