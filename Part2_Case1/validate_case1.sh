#!/usr/bin/env bash
#
# Part II Case 1 -- comprehensive validation.
#
# Exercises the fault injection and the restart-from-zero retry logic over a
# range of failure probabilities and checks, for every run:
#   * the client exits successfully,
#   * every source file is present in the output directory,
#   * no .part leftovers remain,
#   * the SHA-256 of every file matches the source.
#
# Usage: ./validate_case1.sh [runs_per_setting]
#
set -u

RUNS=${1:-3}
DATASET_DIR=${DATASET_DIR:-../raw-img}
TOPIC=${TOPIC:-mucca}
PORT_BASE=${PORT_BASE:-9300}
WORK=$(mktemp -d /tmp/case1_validate.XXXXXX)

make clean >/dev/null 2>&1
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

hash_dir() {
    if command -v sha256sum >/dev/null 2>&1; then
        ( cd "$1" && find . -type f -exec sha256sum {} + | sort )
    else
        ( cd "$1" && find . -type f -exec shasum -a 256 {} + | sort )
    fi
}

echo "Precomputing expected SHA-256 for dataset $TOPIC..."
hash_dir "$DATASET_DIR/$TOPIC" > "$WORK/exp.txt"

TOTAL_PASS=0
TOTAL_FAIL=0

check_run() {
    local p=$1 outdir=$2 server_log=$3 client_log=$4 label=$5

    local src_count dst_count
    src_count=$(find "$DATASET_DIR/$TOPIC" -type f | wc -l | tr -d ' ')
    dst_count=$(find "$outdir/$TOPIC" -type f 2>/dev/null | wc -l | tr -d ' ')

    local leftovers
    leftovers=$(find "$outdir/$TOPIC" -name '*.part' 2>/dev/null | wc -l | tr -d ' ')

    hash_dir "$outdir/$TOPIC" 2>/dev/null > "$WORK/act.txt"

    local faults attempts redundant
    faults=$(grep -c "FAULT conn=" "$server_log" 2>/dev/null || echo 0)
    attempts=$(grep -o "[0-9]* attempts" "$client_log" 2>/dev/null | head -1 | cut -d' ' -f1)
    attempts=${attempts:-0}
    redundant=$(grep -o "[0-9]* redundant" "$client_log" 2>/dev/null | head -1 | cut -d' ' -f1)
    redundant=${redundant:-0}

    if diff -q "$WORK/exp.txt" "$WORK/act.txt" >/dev/null 2>&1 \
       && [ "$src_count" = "$dst_count" ] && [ "$leftovers" = "0" ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
        printf "  [PASS] %-16s files=%s faults=%s attempts=%s redundant=%s bytes\n" \
               "$label" "$dst_count" "$faults" "$attempts" "$redundant"
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
        printf "  [FAIL] %-16s files=%s/%s leftovers=%s faults=%s attempts=%s\n" \
               "$label" "$dst_count" "$src_count" "$leftovers" "$faults" "$attempts"
    fi
}

for p in 0.0 0.2 0.5 0.8; do
    echo "--- failure probability p = $p ---"
    pidx=${p/./}          # "0.5" -> "05", used for the port offset
    for r in $(seq 1 $RUNS); do
        port=$((PORT_BASE + 10#$pidx * 10 + r))
        outdir="$WORK/out_${p}_${r}"
        slog="$WORK/srv_${p}_${r}.log"
        clog="$WORK/cli_${p}_${r}.log"
        mkdir -p "$outdir"

        ./server "$port" "$DATASET_DIR" "$p" log=2 > "$slog" 2>&1 &
        spid=$!
        sleep 0.8
        ./client 127.0.0.1 "$port" "$TOPIC" "$outdir" log=2 metrics=none > "$clog" 2>&1
        rc=$?
        kill -TERM $spid 2>/dev/null || true
        wait $spid 2>/dev/null || true

        if [ "$rc" -ne 0 ]; then
            TOTAL_FAIL=$((TOTAL_FAIL+1))
            printf "  [FAIL] %-16s client exit code %s\n" "p=$p run=$r" "$rc"
            continue
        fi
        check_run "$p" "$outdir" "$slog" "$clog" "p=$p run=$r"
    done
done

echo ""
echo "=================================================="
echo " TOTAL: $TOTAL_PASS passed, $TOTAL_FAIL failed"
echo "=================================================="
[ "$TOTAL_FAIL" -eq 0 ]
