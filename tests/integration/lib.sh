#!/usr/bin/env bash
# Shared helpers for the sds3sync integration test harness. Sourced by
# each scenario script (run_*.sh), never run directly.
set -euo pipefail

INTEGRATION_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$INTEGRATION_DIR/../../app" && pwd)"
SCRATCH_ROOT="${SCRATCH_ROOT:-$(mktemp -d /tmp/sds3sync-it.XXXXXX)}"

# The daemon under test talks to MinIO *through* toxiproxy, so fault
# injection (added via the control API on 8474) actually affects it.
# Verification talks to MinIO directly, so it stays reliable regardless
# of whatever toxics a scenario currently has configured.
MINIO_ENDPOINT="${MINIO_ENDPOINT:-http://127.0.0.1:9010}"
MINIO_DIRECT_ENDPOINT="${MINIO_DIRECT_ENDPOINT:-http://127.0.0.1:9000}"
TOXIPROXY_API="${TOXIPROXY_API:-http://127.0.0.1:8474}"
MINIO_BUCKET="${MINIO_BUCKET:-sds3sync-it}"
MINIO_ACCESS_KEY="${MINIO_ACCESS_KEY:-testkey}"
MINIO_SECRET_KEY="${MINIO_SECRET_KEY:-testsecret123}"

log() { echo "[$(date -u +%H:%M:%S)] $*" >&2; }

wait_for_minio() {
    log "waiting for MinIO..."
    for _ in $(seq 1 60); do
        if curl -sf "$MINIO_DIRECT_ENDPOINT/minio/health/live" >/dev/null 2>&1; then
            log "MinIO is up"
            return 0
        fi
        sleep 1
    done
    log "MinIO did not become healthy in time"
    return 1
}

wait_for_toxiproxy() {
    log "waiting for toxiproxy..."
    for _ in $(seq 1 60); do
        if curl -sf "$TOXIPROXY_API/proxies" >/dev/null 2>&1; then
            log "toxiproxy is up"
            return 0
        fi
        sleep 1
    done
    log "toxiproxy did not become healthy in time"
    return 1
}

mc_alias() {
    mc alias set sds3sync-it "$MINIO_DIRECT_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" >/dev/null
}

ensure_bucket() {
    mc_alias
    mc mb --ignore-existing "sds3sync-it/$MINIO_BUCKET" >/dev/null
}

bucket_object_count() {
    mc_alias
    mc ls --recursive "sds3sync-it/$MINIO_BUCKET" 2>/dev/null | wc -l | tr -d ' '
}

bucket_has_object() {
    local key="$1"
    mc_alias
    mc stat "sds3sync-it/$MINIO_BUCKET/$key" >/dev/null 2>&1
}

# Seed a synthetic .mkv file with a backdated mtime (default: 10 minutes
# in the past, well past MIN_AGE_SECONDS=120) so the daemon considers it
# finished immediately -- no idle sleep needed anywhere in the tests, and
# deliberately not touching MIN_AGE_SECONDS itself (see README: hardcoded
# on purpose, against silent misconfiguration).
seed_recording() {
    local dir="$1" relpath="$2" size_bytes="${3:-65536}" age_seconds="${4:-600}"
    local full="$dir/$relpath"
    mkdir -p "$(dirname "$full")"
    head -c "$size_bytes" /dev/urandom > "$full"
    touch -d "@$(( $(date +%s) - age_seconds ))" "$full"
}

# Launch the real sds3sync-host binary as a background process, pointed at
# a fresh scratch param file + state file + recording dir. Each named test
# gets its own subtree under SCRATCH_ROOT and its own bucket Prefix so
# scenarios never collide with each other's objects. Prints the PID.
start_daemon() {
    local test_name="$1" recording_dir="$2" interval="${3:-3}"
    local run_dir="$SCRATCH_ROOT/$test_name"
    mkdir -p "$run_dir"

    local param_file="$run_dir/params.conf"
    cat > "$param_file" <<EOF
S3Endpoint=$MINIO_ENDPOINT
S3Bucket=$MINIO_BUCKET
S3Region=us-east-1
S3AccessKey=$MINIO_ACCESS_KEY
S3SecretKey=$MINIO_SECRET_KEY
S3PathStyle=yes
S3InsecureTLS=no
Prefix=$test_name/
RecordingPath=$recording_dir
IntervalSeconds=$interval
EOF

    SDS3SYNC_PARAM_FILE="$param_file" \
    SDS3SYNC_STATE_PATH="$run_dir/uploaded.txt" \
        "$APP_DIR/sds3sync-host" > "$run_dir/daemon.log" 2>&1 &
    local pid=$!
    echo "$pid" > "$run_dir/pid"
    echo "$pid"
}

# Same as start_daemon but resumes an existing run_dir (same param file,
# same state file) -- used to test restart-after-crash behavior.
restart_daemon() {
    local test_name="$1"
    local run_dir="$SCRATCH_ROOT/$test_name"
    local param_file="$run_dir/params.conf"

    SDS3SYNC_PARAM_FILE="$param_file" \
    SDS3SYNC_STATE_PATH="$run_dir/uploaded.txt" \
        "$APP_DIR/sds3sync-host" >> "$run_dir/daemon.log" 2>&1 &
    local pid=$!
    echo "$pid" > "$run_dir/pid"
    echo "$pid"
}

stop_daemon() {
    local pid="$1"
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

kill_daemon() {
    local pid="$1"
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

is_alive() {
    local pid="$1"
    kill -0 "$pid" 2>/dev/null
}

rss_kb() {
    local pid="$1"
    awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo ""
}

thread_count() {
    local pid="$1"
    awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null || echo ""
}

# Waits until the daemon's log shows at least $2 "sync pass done" lines.
wait_for_pass() {
    local log_file="$1" n="$2" timeout="${3:-40}"
    for _ in $(seq 1 "$timeout"); do
        local count
        count=$(grep -c "sync pass done" "$log_file" 2>/dev/null || echo 0)
        if [ "$count" -ge "$n" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

pass_count() {
    local log_file="$1"
    grep -c "sync pass done" "$log_file" 2>/dev/null || echo 0
}
