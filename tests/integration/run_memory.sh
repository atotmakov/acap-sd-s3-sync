#!/usr/bin/env bash
# Memory scenario: two phases, not one.
#
#   Churn:    continuously add new recordings, let them upload, RSS grows
#             legitimately as the tracked corpus grows -- this phase just
#             sanity-checks growth isn't wildly disproportionate and logs
#             the RSS-per-tracked-entry trend for visibility. It is not
#             the primary gate (see plateau below): with the small sample
#             counts a quick CI run can afford, per-entry byte estimates
#             here are too noisy (RSS granularity in KB, one-time
#             allocations) to gate tightly on.
#   Plateau:  stop adding files, keep running. Tracked-entry count is now
#             constant, so there is no legitimate reason for RSS to keep
#             growing -- this is the real, tight-tolerance gate. Compares
#             the average of the first half of plateau samples against
#             the second half.
#
# Sizing is pass-count-driven (CHURN_PASSES / PLATEAU_PASSES env vars),
# not wall-clock -- a fast IntervalSeconds accumulates many passes in a
# short CI window; the same script scales up for a longer, realistic-
# cadence nightly soak by overriding these and IntervalSeconds.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

TEST_NAME="memory"
# Kept modest by default since IntervalSeconds has a real 10s floor (see
# below) -- 8+10 passes is ~3 CI-minutes. Override both upward for a more
# thorough (nightly/scheduled) run.
CHURN_PASSES="${CHURN_PASSES:-8}"
PLATEAU_PASSES="${PLATEAU_PASSES:-10}"
FILES_PER_CHURN_PASS="${FILES_PER_CHURN_PASS:-3}"
# load_config() clamps IntervalSeconds to a 10s floor in production code
# (deliberately, "protect against hammering the SD card") -- this isn't
# something tests should route around, so 10 is the real fastest cadence
# available, not a chosen default.
INTERVAL="${MEMORY_INTERVAL_SECONDS:-10}"
# Plateau RSS growth (second-half avg vs first-half avg) above this % is
# a fail. Generous enough to absorb sampling/measurement noise while still
# catching a real per-pass leak, which compounds far faster than this.
PLATEAU_GROWTH_BUDGET_PCT="${PLATEAU_GROWTH_BUDGET_PCT:-15}"

dump_logs() {
    local pid="$1"
    # Diagnostics first, before anything kills the process -- is_alive
    # and the gdb backtrace are only meaningful while it's still running.
    dump_process_diagnostics "$pid"
    echo "--- syslog (pid $pid), tail ---" >&2
    daemon_syslog "$pid" | tail -40 >&2 || true
    echo "--- crash.log ---" >&2
    cat "$SCRATCH_ROOT/$TEST_NAME/crash.log" >&2 2>/dev/null || true
}

wait_for_minio
wait_for_toxiproxy
ensure_bucket

RECORDING_DIR="$SCRATCH_ROOT/$TEST_NAME/recordings"
mkdir -p "$RECORDING_DIR"

PID=$(start_daemon "$TEST_NAME" "$RECORDING_DIR" "$INTERVAL")
log "daemon started, pid=$PID"

churn_rss=()
churn_entries=()
plateau_rss=()

seq_n=0
log "churn phase: $CHURN_PASSES passes, $FILES_PER_CHURN_PASS new files each"
for i in $(seq 1 "$CHURN_PASSES"); do
    for j in $(seq 1 "$FILES_PER_CHURN_PASS"); do
        seq_n=$((seq_n + 1))
        seed_recording "$RECORDING_DIR" "churn_${seq_n}.mkv"
    done
    if ! wait_for_pass "$PID" "$i" 30; then
        log "FAIL: churn pass $i did not complete within timeout"
        dump_logs "$PID"
        kill_daemon "$PID"
        exit 1
    fi
    if ! is_alive "$PID"; then
        log "FAIL: daemon died during churn phase (pass $i)"
        dump_logs "$PID"
        exit 1
    fi
    r=$(rss_kb "$PID"); e=$(tracked_entry_count "$TEST_NAME")
    churn_rss+=("$r"); churn_entries+=("$e")
    log "churn pass $i: rss=${r}KB tracked=${e}"
done

churn_done=$seq_n
log "plateau phase: $PLATEAU_PASSES passes, zero new files"
base_pass=$CHURN_PASSES
for i in $(seq 1 "$PLATEAU_PASSES"); do
    if ! wait_for_pass "$PID" "$((base_pass + i))" 30; then
        log "FAIL: plateau pass $i did not complete within timeout"
        dump_logs "$PID"
        kill_daemon "$PID"
        exit 1
    fi
    if ! is_alive "$PID"; then
        log "FAIL: daemon died during plateau phase (pass $i)"
        dump_logs "$PID"
        exit 1
    fi
    r=$(rss_kb "$PID")
    plateau_rss+=("$r")
    log "plateau pass $i: rss=${r}KB tracked=$(tracked_entry_count "$TEST_NAME")"
done

stop_daemon "$PID"

# --- Churn: log-only trend, not a hard gate (see header comment) ---
if [ "${#churn_rss[@]}" -gt 0 ]; then
    first_r=${churn_rss[0]}; last_r=${churn_rss[-1]}
    first_e=${churn_entries[0]}; last_e=${churn_entries[-1]}
    log "churn trend: rss ${first_r}KB -> ${last_r}KB, tracked ${first_e} -> ${last_e}"
fi

# --- Plateau: the real gate. Discard nothing extra (already short and
# fully post-churn); compare first half vs second half. ---
n=${#plateau_rss[@]}
half=$((n / 2))
if [ "$half" -lt 3 ]; then
    log "FAIL: not enough plateau samples ($n) for a meaningful comparison"
    exit 1
fi

first_half_avg=$(printf '%s\n' "${plateau_rss[@]:0:$half}" | avg)
second_half_avg=$(printf '%s\n' "${plateau_rss[@]: -$half}" | avg)

log "plateau RSS: first half avg=${first_half_avg}KB, second half avg=${second_half_avg}KB"

if [ "$first_half_avg" -le 0 ]; then
    log "FAIL: implausible plateau RSS baseline (${first_half_avg}KB)"
    exit 1
fi

growth_pct=$(( (second_half_avg - first_half_avg) * 100 / first_half_avg ))
log "plateau RSS growth: ${growth_pct}% (budget: ${PLATEAU_GROWTH_BUDGET_PCT}%)"

if [ "$growth_pct" -gt "$PLATEAU_GROWTH_BUDGET_PCT" ]; then
    log "FAIL: plateau RSS grew ${growth_pct}%, over the ${PLATEAU_GROWTH_BUDGET_PCT}% budget -- possible leak (tracked-entry count was constant, so this isn't corpus growth)"
    exit 1
fi

log "PASS: plateau RSS stable (${growth_pct}% <= ${PLATEAU_GROWTH_BUDGET_PCT}% budget)"
exit 0
