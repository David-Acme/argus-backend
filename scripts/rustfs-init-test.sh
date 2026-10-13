#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: ARGUS_RUSTFS_BIN=/path/to/rustfs scripts/rustfs-init-test.sh

Runs scripts/lib/s3-init.sh, the rustfs-init job, against an isolated RustFS
on a free loopback port (never 9000 or 9001) with a temporary data directory
and secrets generated straight into files, the way provision-host.sh writes
them. Every program the job starts goes through a shim that logs its
arguments and its environment, and the job runs under sh -x. The test fails
unless the bucket and the application service account are created, a second
run succeeds unchanged, the account reads and writes inside its bucket and is
refused another bucket, and no key appears in any argument, any environment
or the trace.

Exits 77 (skipped) when ARGUS_RUSTFS_BIN is not set.
USAGE
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
rustfs="${ARGUS_RUSTFS_BIN:-}"
if [ -z "$rustfs" ]; then
  printf 'rustfs-init-test: skipped, ARGUS_RUSTFS_BIN must name the rustfs binary\n'
  exit 77
fi
[ -x "$rustfs" ] || { printf 'rustfs-init-test: %s is not executable\n' "$rustfs" >&2; exit 2; }

work="$(mktemp -d)"
chmod 700 "$work"
pid=""
cleanup() {
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

fail() {
  printf 'rustfs-init-test: %s\n' "$*" >&2
  exit 1
}

port_free() {
  ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

port=""
for _ in $(seq 1 50); do
  candidate=$(( 20000 + RANDOM % 40000 ))
  case "$candidate" in 9000|9001) continue ;; esac
  if port_free "$candidate"; then
    port="$candidate"
    break
  fi
done
[ -n "$port" ] || fail "no free port"
endpoint="http://127.0.0.1:$port"

secrets="$work/secrets"
mkdir -p "$secrets" "$work/data" "$work/logs" "$work/shims"
(umask 077
  openssl rand -hex 20 | tr '[:lower:]' '[:upper:]' > "$secrets/rustfs_access_key"
  openssl rand -hex 48 > "$secrets/rustfs_secret_key"
  openssl rand -hex 48 > "$secrets/rustfs_rpc_secret"
  openssl rand -hex 8 | tr '[:lower:]' '[:upper:]' > "$secrets/argus_s3_access_key"
  openssl rand -hex 16 > "$secrets/argus_s3_secret_key"
  printf 'argus-%s-private\n' "$(openssl rand -hex 10)" > "$secrets/rustfs-bucket")

RUSTFS_ADDRESS="127.0.0.1:$port" \
RUSTFS_CONSOLE_ENABLE=false \
RUSTFS_ACCESS_KEY_FILE="$secrets/rustfs_access_key" \
RUSTFS_SECRET_KEY_FILE="$secrets/rustfs_secret_key" \
RUSTFS_RPC_SECRET="$(openssl rand -hex 24)" \
  "$rustfs" server "$work/data" > "$work/rustfs.log" 2>&1 &
pid=$!
ready=""
for _ in $(seq 1 300); do
  if curl -fsS "$endpoint/health/ready" >/dev/null 2>&1; then
    ready=1
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
done
[ -n "$ready" ] || { tail -n 20 "$work/rustfs.log" >&2; fail "RustFS did not become ready"; }

for tool in curl tr sed grep mktemp rm cat; do
  real="$(command -v "$tool")"
  printf '#!/bin/sh\nprintf "%%s\\n" "%s $*" >> "%s/argv"\nenv >> "%s/env"\nexec %s "$@"\n' \
    "$tool" "$work/logs" "$work/logs" "$real" > "$work/shims/$tool"
  chmod 700 "$work/shims/$tool"
done

run_init() {
  env PATH="$work/shims:$PATH" \
    RUSTFS_ENDPOINT="$endpoint" \
    RUSTFS_REGION=us-east-1 \
    RUSTFS_BUCKET_FILE="$secrets/rustfs-bucket" \
    RUSTFS_SECRETS_DIR="$secrets" \
    sh -x "$ROOT/scripts/lib/s3-init.sh" >> "$work/logs/out" 2>> "$work/logs/trace"
}

run_init || { cat "$work/logs/out" "$work/logs/trace" >&2; fail "the first run failed"; }
grep -q 'are ready' "$work/logs/out" || fail "the first run did not report the account ready"
run_init || { cat "$work/logs/out" >&2; fail "a second run failed"; }
[ "$(grep -c 'are ready' "$work/logs/out")" = 2 ] || fail "the second run did not report the account ready"

bucket="$(tr -d '\r\n' < "$secrets/rustfs-bucket")"
{
  printf 'user = "'
  tr -d '\r\n' < "$secrets/argus_s3_access_key"
  printf ':'
  tr -d '\r\n' < "$secrets/argus_s3_secret_key"
  printf '"\n'
} > "$work/app.curl"
{
  printf 'user = "'
  tr -d '\r\n' < "$secrets/rustfs_access_key"
  printf ':'
  tr -d '\r\n' < "$secrets/rustfs_secret_key"
  printf '"\n'
} > "$work/root.curl"
s3() {
  local who="$1" method="$2" path="$3"
  shift 3
  curl -s -o /dev/null -w '%{http_code}' -K "$work/$who.curl" \
    --aws-sigv4 "aws:amz:us-east-1:s3" -X "$method" "$@" "$endpoint$path"
}
[ "$(s3 app PUT "/$bucket/portraits/a.jpg" --data-binary probe)" = 200 ] \
  || fail "the application account cannot write in its bucket"
[ "$(s3 app GET "/$bucket/portraits/a.jpg")" = 200 ] \
  || fail "the application account cannot read in its bucket"
[ "$(s3 root PUT "/argus-other-bucket")" = 200 ] || fail "the second bucket could not be created"
[ "$(s3 app PUT "/argus-other-bucket/a.jpg" --data-binary probe)" = 403 ] \
  || fail "the application account was not confined to its bucket"

leaks=0
for name in rustfs_access_key rustfs_secret_key argus_s3_access_key argus_s3_secret_key; do
  value="$(tr -d '\r\n' < "$secrets/$name")"
  for log in argv env trace out; do
    if grep -qF -- "$value" "$work/logs/$log"; then
      printf 'rustfs-init-test: %s appears in the %s log\n' "$name" "$log" >&2
      leaks=$(( leaks + 1 ))
    fi
  done
done
[ "$leaks" -eq 0 ] || fail "$leaks key exposure(s)"
[ -s "$work/logs/argv" ] || fail "the shims recorded no program, so the argument check proved nothing"
grep -q '^curl ' "$work/logs/argv" || fail "the shims did not see curl"

printf 'rustfs-init-test: bucket and account ready twice, account confined to its bucket, no key in %s arguments, environments or the trace\n' \
  "$(wc -l < "$work/logs/argv" | tr -d ' ')"
