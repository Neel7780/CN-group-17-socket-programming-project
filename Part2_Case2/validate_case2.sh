#!/usr/bin/env bash
#
# Part II Case 2 -- comprehensive validation.
#
# Exercises the session management and resume protocol across a range of
# fault injection rates and checks, for every run:
#   * the client exits successfully,
#   * every source file is present in the output directory,
#   * no .part or .ckpt leftovers remain,
#   * the SHA-256 of every file matches the source.
#
# Usage: ./validate_case2.sh [runs_per_setting]
#
set -u

RUNS=${1:-3}
DATASET_DIR=${DATASET_DIR:-../raw-img}
TOPIC=${TOPIC:-mucca}
PORT_BASE=${PORT_BASE:-9300}
WORK=$(mktemp -d /tmp/case2_validate.XXXXXX)

make clean >/dev/null 2>&1
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

TOTAL_PASS=0
TOTAL_FAIL=0

check_run() {
    local p=$1 port=$2 outdir=$3 server_log=$4 client_log=$5 label=$6

    local src_count dst_count
    src_count=$(find "$DATASET_DIR/$TOPIC" -type f | wc -l | tr -d ' ')
    dst_count=$(find "$outdir/$TOPIC" -type f 2>/dev/null | wc -l | tr -d ' ')

    local leftovers
    leftovers=$(find "$outdir/$TOPIC" \( -name '*.part' -o -name '.ckpt*' \) 2>/dev/null | wc -l | tr -d ' ')

    ( cd "$DATASET_DIR/$TOPIC" && find . -type f -exec shasum -a 256 {} \; | sort ) > "$WORK/exp.txt"
    ( cd "$outdir/$TOPIC" 2>/dev/null && find . -type f -exec shasum -a 256 {} \; | sort ) > "$WORK/act.txt"

    local resume_ok resume_rej faults redundant
    resume_ok=$(grep -c "RESUME accepted" "$server_log" 2>/dev/null || echo 0)
    resume_rej=$(grep -c "RESUME rejected" "$server_log" 2>/dev/null || echo 0)
    faults=$(grep -c "FAULT executed" "$server_log" 2>/dev/null || echo 0)
    redundant=$(grep -o "[0-9]* redundant" "$client_log" 2>/dev/null | head -1 | cut -d' ' -f1)
    redundant=${redundant:-0}

    if diff -q "$WORK/exp.txt" "$WORK/act.txt" >/dev/null 2>&1 \
       && [ "$src_count" = "$dst_count" ] && [ "$leftovers" = "0" ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
        printf "  [PASS] %-22s files=%s faults=%s resumes=%s rejected=%s redundant=%s bytes\n" \
               "$label" "$dst_count" "$faults" "$resume_ok" "$resume_rej" "$redundant"
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
        printf "  [FAIL] %-22s files=%s/%s leftovers=%s faults=%s resumes=%s rejected=%s\n" \
               "$label" "$dst_count" "$src_count" "$leftovers" "$faults" "$resume_ok" "$resume_rej"
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

        ./server "$port" "$DATASET_DIR" "$p" log=1 > "$slog" 2>&1 &
        spid=$!
        sleep 0.8
        ./client 127.0.0.1 "$port" "$TOPIC" "$outdir" log=1 > "$clog" 2>&1
        rc=$?
        kill -TERM $spid 2>/dev/null || true
        wait $spid 2>/dev/null || true

        if [ "$rc" -ne 0 ]; then
            TOTAL_FAIL=$((TOTAL_FAIL+1))
            printf "  [FAIL] %-22s client exit code %s\n" "p=$p run=$r" "$rc"
            continue
        fi
        check_run "$p" "$port" "$outdir" "$slog" "$clog" "p=$p run=$r"
    done
done

echo ""
echo "=================================================="
echo " TOTAL: $TOTAL_PASS passed, $TOTAL_FAIL failed"
echo "=================================================="
[ "$TOTAL_FAIL" -eq 0 ]
