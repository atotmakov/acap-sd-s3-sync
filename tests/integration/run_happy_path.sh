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

# index.db is the camera's own live index of what's currently on the SD
# card, rewritten in place as recordings are added/removed. It must never
# be synced (see README: mirroring it onto S3 would erase index entries for
# recordings still in the bucket but rotated off the card).
seed_recording "$RECORDING_DIR" "index.db"

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
INDEX_SYNCED=0
if bucket_has_object "happy-path/index.db"; then
    INDEX_SYNCED=1
fi
stop_daemon "$PID"

if [ "$COUNT" -eq "$N" ] && [ "$INDEX_SYNCED" -eq 0 ]; then
    log "PASS: all $N synthetic recordings landed in the bucket, index.db excluded"
    exit 0
else
    log "FAIL: expected $N objects in bucket (found $COUNT), index.db excluded (synced=$INDEX_SYNCED)"
    dump_logs "$PID"
    exit 1
fi
