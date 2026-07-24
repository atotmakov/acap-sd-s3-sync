#!/usr/bin/env bash
# Happy-path end-to-end check: seed synthetic recordings, run the real
# sds3sync-host binary against MinIO through toxiproxy (no toxics active),
# confirm every file lands in the bucket. This is the harness's own
# self-test -- proves the plumbing (host build, param file, toxiproxy
# passthrough, syslog capture, bucket verification) actually works before
# any of the fault-injection scenarios build on top of it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

dump_logs() {
    local pid="$1" run_dir="$SCRATCH_ROOT/happy-path"
    echo "--- syslog (pid $pid) ---" >&2
    daemon_syslog "$pid" >&2 || true
    echo "--- crash.log ---" >&2
    cat "$run_dir/crash.log" >&2 2>/dev/null || true
}

wait_for_minio
wait_for_toxiproxy
ensure_bucket

RECORDING_DIR="$SCRATCH_ROOT/happy-path/recordings"
mkdir -p "$RECORDING_DIR"

N=5
for i in $(seq 1 "$N"); do
    seed_recording "$RECORDING_DIR" "clip_$i.mkv"
done

PID=$(start_daemon "happy-path" "$RECORDING_DIR" 3)
log "daemon started, pid=$PID"

if ! wait_for_pass "$PID" 1 40; then
    log "FAIL: no sync pass completed within timeout"
    dump_logs "$PID"
    kill_daemon "$PID"
    exit 1
fi

sleep 2 # let the last pass's uploads settle
COUNT=$(bucket_object_count)
stop_daemon "$PID"

if [ "$COUNT" -eq "$N" ]; then
    log "PASS: all $N synthetic recordings landed in the bucket"
    exit 0
else
    log "FAIL: expected $N objects in bucket, found $COUNT"
    dump_logs "$PID"
    exit 1
fi
