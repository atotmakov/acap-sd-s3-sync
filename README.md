# sds3sync — Axis ACAP: push SD-card recordings to S3-compatible storage

Native ACAP application for Axis cameras (built for AXIS M3085-V, Ambarella
CV25, aarch64, AXIS OS 12.x) that uploads finished recordings from the SD card
to an S3-compatible object store (MinIO, Backblaze B2, or any SigV4 endpoint).

## Design

- **Trigger**: an in-app monotonic timer (`IntervalSeconds`, default 60 s)
  fires one sync pass per tick; the first pass runs 15 s after app start. No
  camera-side schedule/pulse configuration is needed. Ticks that arrive while
  a pass is still running are skipped.
- **Sync pass**: recursively scans `SourceDir` (default
  `/var/spool/storage/SD_DISK`), uploads every `.mkv` file (hardcoded — see
  below) whose mtime is at least `MIN_AGE_SECONDS` (120 s, hardcoded — see
  below) old, skipping chunks still being written, that is not already
  recorded in the local state file. Upload once, keep on SD; the camera's
  own FIFO cleanup reclaims space.
- **State**: `/usr/local/packages/sds3sync/localdata/uploaded.txt`, one
  `size<TAB>relative-path` line per uploaded file, mirrored into an in-memory
  hash table (relative path → size) loaded at startup. Delete the file and
  restart the app to re-upload everything. If a file's size changes after
  upload (should not happen for finalized chunks) it is re-uploaded and
  overwritten. Survives app upgrade/reinstall (`localdata` isn't purged by a
  plain `remove`); only a factory-reset-style wipe clears it.

  Neither structure is aware of what's still physically on the SD card, so
  without pruning both would grow forever — finished files are never deleted
  from SD (the camera's own FIFO cleanup reclaims space), and every unique
  path ever uploaded stays tracked regardless of whether its file was later
  cycled out. A **reconcile pass** keeps both bounded by "what currently fits
  on the SD card" instead of "everything ever uploaded since deployment": it
  drops any tracked entry whose file no longer exists under `SourceDir`, then
  atomically rewrites `uploaded.txt` from what's left (temp file + rename —
  a crash mid-rewrite leaves the previous state file intact). Reconcile runs
  (a) once at startup, right after loading state — catches cruft accumulated
  across restarts — and (b) every 1024 successful uploads during normal
  operation. If `SourceDir` itself isn't accessible when a reconcile would
  run (e.g. SD card not yet mounted), the whole pass is skipped rather than
  risk wiping entries for files that are only temporarily invisible.
  Verified against a real deletion (removed a recording via VAPIX
  `record/remove.cgi`, restarted the app, confirmed the log reported
  exactly one stale entry pruned and the state file rewritten).
- **Uploads**: HTTPS PUT with AWS Signature V4, `UNSIGNED-PAYLOAD`, streamed
  via libcurl (no file buffering in RAM). A pass aborts after 3 consecutive
  failures and retries on the next timer tick. One pass runs at a time;
  overlapping ticks are skipped.

## Parameters (Apps → SD to S3 Sync → Settings)

Two groups: **connection** settings (how to reach and address the S3-compatible
endpoint — prefixed `S3*`) and **operation** settings (how the sync itself
behaves on this camera).

| Param | Default | Meaning |
|---|---|---|
| `S3Endpoint` | *(empty — required)* | e.g. `https://minio.example.com:9000` |
| `S3Bucket` | `cctv` | Target bucket (must exist) |
| `S3Region` | `us-east-1` | SigV4 region string (MinIO accepts the default) |
| `S3AccessKey` / `S3SecretKey` | *(empty — required)* | Credentials. Stored as plain ACAP params — create a scoped, write-only key |
| `S3PathStyle` | `yes` | `yes` = `endpoint/bucket/key` (MinIO); `no` = virtual-host style |
| `S3InsecureTLS` | `no` | `yes` = skip TLS cert verification (self-signed MinIO) |
| `Prefix` | *(empty — auto-derived)* | Object key prefix. If empty on first run, derived from `/etc/hostname` (falls back to `axis-<eth0 MAC>` if unreadable) and **persisted back** to this parameter — check the app's Settings page after first start to see what it picked, or set it explicitly to override. |
| `SourceDir` | `/var/spool/storage/SD_DISK` | Directory tree to sync |
| `IntervalSeconds` | `60` | Sync pass cadence (clamped to ≥ 10) |

Two things are hardcoded in `sdsync.c` rather than exposed as parameters,
both for the same reason — they're not operational choices, and a
misconfigured value fails silently (zero uploads, indistinguishable from "no
new recordings"), so they're not worth exposing as something to get wrong:

- **`RECORDING_EXTENSION`** (`.mkv`) — dictated by the camera's own
  recording engine. If this app is ever installed on a camera generation
  that writes a different container format, change the constant and rebuild.
- **`MIN_AGE_SECONDS`** (`120`) — a safety margin against the recording
  pipeline's own chunking/flush behavior, not something that should vary by
  deployment.

The app starts idle if `S3Endpoint`/`S3AccessKey`/`S3SecretKey` are unset —
configure them, then restart the app.

## Build

Requires Docker (the ACAP Native SDK toolchain is distributed as a Docker
image). From this directory:

```bash
docker buildx build --build-arg ARCH=aarch64 --output type=local,dest=build .
# .eap package lands in build/opt/app/
```

Pin `--build-arg SDK_VERSION=` to a specific `axisecp/acap-native-sdk` tag if
`latest` breaks; AXIS OS 12.x pairs with SDK 12.x.

## Install

Camera UI: Apps → `+ Add app` → pick the `.eap`. Or via VAPIX:

```bash
curl --digest -u root:<pw> -F packfil=@build/opt/app/sds3sync_0_1_0_aarch64.eap \
  "http://<camera-ip>/axis-cgi/applications/upload.cgi"
curl --digest -u root:<pw> "http://<camera-ip>/axis-cgi/applications/control.cgi?action=start&package=sds3sync"
```

## Logs / troubleshooting

App logs go to the camera syslog:

```bash
curl -s --digest -u root:<pw> "http://<camera-ip>/axis-cgi/admin/systemlog.cgi" | grep sds3sync
```

- `idle: waiting for configuration` — set S3Endpoint/S3AccessKey/S3SecretKey, restart app.
- Uploads failing with HTTP 403 — check clock (SigV4 is time-sensitive; NTP
  must work), credentials, and bucket policy.
- No SD access (`g_dir_open` failures / zero files found) — the manifest
  requests the `storage` Linux group; AXIS OS 12 tightened ACAP storage
  sandboxing, so if the app still cannot read
  `/var/spool/storage/SD_DISK`, this is the first thing to investigate
  (see manifest `resources.linux.user.groups`).
- SigV4 here signs only `host`, `x-amz-content-sha256`, `x-amz-date` with
  `UNSIGNED-PAYLOAD` — fine for MinIO/B2 over TLS; AWS S3 proper also accepts
  it, but signed-payload support would need to be added for policies that
  require `x-amz-content-sha256` to be a real digest.
