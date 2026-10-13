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
unless the bucket, the shared application account and one account per writer
(identity, camera, guard) are created, each writer's server-generated keys
written to its own 0600 credentials file; each writer's account reads, writes,
deletes and lists under its own prefixes only; a second run keeps every key; an
account lost on the server is created again; a lost credentials file gets a new
account and the old one is removed; the shared account works inside its bucket
only; and no key appears in any argument, any environment or the trace.

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
accounts="$work/accounts"
for service in identity camera guard; do
  mkdir -p "$accounts/$service"
done
chmod -R 700 "$accounts"
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
    RUSTFS_ACCOUNTS_DIR="$accounts" \
    sh -x "$ROOT/scripts/lib/s3-init.sh" >> "$work/logs/out" 2>> "$work/logs/trace"
}

keep_keys() {
  cat "$accounts"/*/credentials.json >> "$work/generated"
}

run_init || { cat "$work/logs/out" "$work/logs/trace" >&2; fail "the first run failed"; }
grep -q 'are ready' "$work/logs/out" || fail "the first run did not report the shared account ready"
for service in identity camera guard; do
  file="$accounts/$service/credentials.json"
  [ -s "$file" ] || fail "no credentials file for $service"
  [ "$(find "$file" -perm 600)" = "$file" ] || fail "the $service credentials file is not 0600"
  grep -q '"accessKey"' "$file" && grep -q '"secretKey"' "$file" || fail "the $service credentials file has no keys"
done
keep_keys
sha256sum "$accounts"/*/credentials.json > "$work/first.sums"
run_init || { cat "$work/logs/out" >&2; fail "a second run failed"; }
[ "$(grep -c 'are ready' "$work/logs/out")" = 2 ] || fail "the second run did not report the shared account ready"
sha256sum -c --quiet "$work/first.sums" || fail "a second run changed a writer's keys"

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
account_curl() {
  awk -F'"' '{ for (i = 1; i < NF; i++) { if ($i == "accessKey") a = $(i + 2); if ($i == "secretKey") s = $(i + 2) } }
    END { printf "user = \"%s:%s\"\n", a, s }' "$accounts/$1/credentials.json" > "$work/$1.curl"
}
failures=0
expect() {
  local want="$1" label="$2" got
  shift 2
  got="$(s3 "$@")"
  if { [ "$want" = 403 ] && [ "$got" = 403 ]; } || { [ "$want" != 403 ] && [ "${got:0:1}" = 2 ]; }; then
    return 0
  fi
  printf 'rustfs-init-test: %s: wanted %s, got %s\n' "$label" "$([ "$want" = 403 ] && echo 403 || echo 2xx)" "$got" >&2
  failures=$(( failures + 1 ))
}
writer_prefixes() {
  case "$1" in
    identity) echo "portraits/ faces/" ;;
    camera) echo "cameras/" ;;
    guard) echo "guard/incidents/" ;;
  esac
}
all_prefixes="portraits/ faces/ cameras/ guard/incidents/"
check_writers() {
  local service prefix own
  for prefix in $all_prefixes; do
    [ "$(s3 root PUT "/$bucket/${prefix}seed.bin" --data-binary seed)" = 200 ] || fail "the seed object under $prefix could not be written"
  done
  for service in identity camera guard; do
    account_curl "$service"
    for prefix in $all_prefixes; do
      own=0
      case " $(writer_prefixes "$service") " in *" $prefix "*) own=1 ;; esac
      if [ "$own" = 1 ]; then
        expect 200 "$service writes under $prefix" "$service" PUT "/$bucket/${prefix}$service.bin" --data-binary probe
        expect 200 "$service reads under $prefix" "$service" GET "/$bucket/${prefix}seed.bin"
        expect 200 "$service lists $prefix" "$service" GET "/$bucket?list-type=2&prefix=$prefix"
        expect 200 "$service deletes under $prefix" "$service" DELETE "/$bucket/${prefix}$service.bin"
      else
        expect 403 "$service writes under $prefix" "$service" PUT "/$bucket/${prefix}$service.bin" --data-binary probe
        expect 403 "$service reads under $prefix" "$service" GET "/$bucket/${prefix}seed.bin"
        expect 403 "$service deletes under $prefix" "$service" DELETE "/$bucket/${prefix}seed.bin"
        expect 403 "$service lists $prefix" "$service" GET "/$bucket?list-type=2&prefix=$prefix"
      fi
    done
    expect 403 "$service writes at the bucket root" "$service" PUT "/$bucket/root.bin" --data-binary probe
    expect 403 "$service lists the whole bucket" "$service" GET "/$bucket?list-type=2"
    expect 403 "$service sets the bucket policy" "$service" PUT "/$bucket?policy" --data-binary '{}'
  done
  expect 403 "guard writes under guard/ outside incidents/" guard PUT "/$bucket/guard/other.bin" --data-binary probe
  [ "$failures" -eq 0 ] || fail "$failures writer request(s) were not answered as the prefix policy says"
}
accounts_named() {
  curl -s -K "$work/root.curl" --aws-sigv4 "aws:amz:us-east-1:s3" "$endpoint/rustfs/admin/v3/list-service-accounts" \
    | grep -o "\"name\":\"$1\"" | wc -l | tr -d ' '
}

[ "$(s3 app PUT "/$bucket/portraits/a.jpg" --data-binary probe)" = 200 ] \
  || fail "the application account cannot write in its bucket"
[ "$(s3 app GET "/$bucket/portraits/a.jpg")" = 200 ] \
  || fail "the application account cannot read in its bucket"
[ "$(s3 root PUT "/argus-other-bucket")" = 200 ] || fail "the second bucket could not be created"
[ "$(s3 app PUT "/argus-other-bucket/a.jpg" --data-binary probe)" = 403 ] \
  || fail "the application account was not confined to its bucket"

check_writers

old_camera="$(awk -F'"' '{ for (i = 1; i < NF; i++) if ($i == "accessKey") print $(i + 2) }' "$accounts/camera/credentials.json")"
printf 'url = "%s/rustfs/admin/v3/delete-service-accounts?accessKey=%s"\n' "$endpoint" "$old_camera" > "$work/delete.curl"
[ "$(curl -s -o /dev/null -w '%{http_code}' -K "$work/root.curl" -K "$work/delete.curl" --aws-sigv4 "aws:amz:us-east-1:s3" -X DELETE)" = 200 ] \
  || fail "the camera account could not be removed for the test"
run_init || { cat "$work/logs/out" >&2; fail "the run after a lost account failed"; }
grep -q '"accessKey":"'"$old_camera"'"' "$accounts/camera/credentials.json" && fail "a lost camera account was not created again"
[ "$(accounts_named argus-camera)" = 1 ] || fail "the camera account is not the only one of its name"
keep_keys

old_guard="$(cat "$accounts/guard/credentials.json")"
rm -f "$accounts/guard/credentials.json"
run_init || { cat "$work/logs/out" >&2; fail "the run after a lost credentials file failed"; }
[ -s "$accounts/guard/credentials.json" ] || fail "a lost guard credentials file was not issued again"
[ "$(cat "$accounts/guard/credentials.json")" != "$old_guard" ] || fail "the guard keys did not change"
[ "$(accounts_named argus-guard)" = 1 ] || fail "the guard account left an orphan behind"
keep_keys
check_writers

awk -F'"' '{ for (i = 1; i < NF; i++) if ($i == "accessKey" || $i == "secretKey") print $(i + 2) }' "$work/generated" \
  | sort -u > "$work/generated.values"
[ "$(wc -l < "$work/generated.values")" -ge 10 ] || fail "too few generated keys were collected"
for log in argv env trace out; do
  if grep -qF -f "$work/generated.values" "$work/logs/$log"; then
    fail "a server-generated key appears in the $log log"
  fi
done

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

printf 'rustfs-init-test: shared and writer accounts ready on four runs, each writer confined to its prefixes, keys kept and re-issued as they should be, no key in %s arguments, environments or the trace\n' \
  "$(wc -l < "$work/logs/argv" | tr -d ' ')"
