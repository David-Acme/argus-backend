#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: ARGUS_RUSTFS_BIN=/path/to/rustfs ARGUS_RC_BIN=/path/to/rc scripts/rustfs-prefix-policy-test.sh

Decides whether RustFS enforces a service account's prefix policy. Starts an
isolated RustFS on a free loopback port with a temporary data directory,
creates one private bucket and two service accounts the way rustfs-init
creates the application account (an inline policy under the root user), each
allowed one prefix (portraits/ and cameras/), and checks with signed S3
requests that each account can list, read, write and delete under its own
prefix and is refused everything else: the other prefix, a prefix that merely
starts with the same letters, a listing outside its prefix, a copy and a
multipart upload across prefixes, and bucket-level calls.

Exit status: 0 when every request is answered as the policy says, 1 when one is
not (RustFS does not enforce the prefix policy), 77 (skipped) when
ARGUS_RUSTFS_BIN or ARGUS_RC_BIN is not set.
USAGE
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
rustfs="${ARGUS_RUSTFS_BIN:-}"
rc="${ARGUS_RC_BIN:-}"
if [ -z "$rustfs" ] || [ -z "$rc" ]; then
  printf 'rustfs-prefix-policy-test: skipped, ARGUS_RUSTFS_BIN and ARGUS_RC_BIN must name the rustfs and rc binaries\n'
  exit 77
fi
for binary in "$rustfs" "$rc"; do
  [ -x "$binary" ] || { printf 'rustfs-prefix-policy-test: %s is not executable\n' "$binary" >&2; exit 2; }
done

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
[ -n "$port" ] || { printf 'rustfs-prefix-policy-test: no free port\n' >&2; exit 1; }

key() {
  openssl rand -hex "$1" | tr '[:lower:]' '[:upper:]'
}

secret() {
  openssl rand -hex "$1"
}

mkdir -p "$work/data" "$work/rc"
(umask 077
  key 10 > "$work/root.access"
  secret 24 > "$work/root.secret")
root_access="$(cat "$work/root.access")"
root_secret="$(cat "$work/root.secret")"
endpoint="http://127.0.0.1:$port"
bucket="argus-proof-$(openssl rand -hex 4)"

RUSTFS_ADDRESS="127.0.0.1:$port" \
RUSTFS_CONSOLE_ENABLE=false \
RUSTFS_ACCESS_KEY_FILE="$work/root.access" \
RUSTFS_SECRET_KEY_FILE="$work/root.secret" \
RUSTFS_RPC_SECRET="$(secret 24)" \
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
if [ -z "$ready" ]; then
  tail -n 20 "$work/rustfs.log" >&2
  printf 'rustfs-prefix-policy-test: RustFS did not become ready\n' >&2
  exit 1
fi

admin() {
  RC_CONFIG_DIR="$work/rc" RC_HOST_proof="http://$root_access:$root_secret@127.0.0.1:$port" "$rc" "$@"
}

admin bucket create "proof/$bucket" > /dev/null

account() {
  local name="$1" prefix="$2" access secret_key
  access="$(key 8)"
  secret_key="$(secret 16)"
  printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:ListBucket"],"Resource":["arn:aws:s3:::%s"],"Condition":{"StringLike":{"s3:prefix":["%s/*"]}}},{"Effect":"Allow","Action":["s3:GetObject","s3:PutObject","s3:DeleteObject"],"Resource":["arn:aws:s3:::%s/%s/*"]}]}\n' \
    "$bucket" "$prefix" "$bucket" "$prefix" > "$work/$name.policy"
  if ! admin admin service-account create proof "$access" "$secret_key" \
      --name "proof-$name" --policy "$work/$name.policy" > /dev/null 2> "$work/rc.err"; then
    cat "$work/rc.err" >&2
    printf 'rustfs-prefix-policy-test: the %s service account could not be created\n' "$name" >&2
    exit 1
  fi
  (umask 077 && printf 'user = "%s:%s"\n' "$access" "$secret_key" > "$work/$name.curl")
}

(umask 077 && printf 'user = "%s:%s"\n' "$root_access" "$root_secret" > "$work/root.curl")
account portraits portraits
account cameras cameras

request() {
  local who="$1" method="$2" path="$3"
  shift 3
  curl -s -o /dev/null -w '%{http_code}' -K "$work/$who.curl" \
    --aws-sigv4 "aws:amz:us-east-1:s3" -X "$method" "$@" "$endpoint/$bucket$path"
}

failures=0
expect() {
  local want="$1" label="$2" got
  shift 2
  got="$(request "$@")"
  if [ "$want" = 403 ] && [ "$got" = 403 ]; then
    printf 'ok    %s (%s)\n' "$label" "$got"
  elif [ "$want" != 403 ] && [ "${got:0:1}" = 2 ]; then
    printf 'ok    %s (%s)\n' "$label" "$got"
  else
    printf 'FAIL  %s: wanted %s, got %s\n' "$label" "$([ "$want" = 403 ] && echo 403 || echo 2xx)" "$got"
    failures=$(( failures + 1 ))
  fi
}

printf 'portrait\n' > "$work/portrait"
printf 'evidence\n' > "$work/evidence"
printf '{"Version":"2012-10-17","Statement":[]}\n' > "$work/bucket-policy"

expect 200 "portraits writes under portraits/" portraits PUT /portraits/a.jpg --data-binary "@$work/portrait"
expect 200 "portraits reads under portraits/" portraits GET /portraits/a.jpg
expect 200 "cameras writes under cameras/" cameras PUT /cameras/1/b.jpg --data-binary "@$work/evidence"
expect 200 "cameras reads under cameras/" cameras GET /cameras/1/b.jpg
expect 200 "portraits lists its prefix" portraits GET "/?list-type=2&prefix=portraits/"
expect 403 "portraits reads under cameras/" portraits GET /cameras/1/b.jpg
expect 403 "portraits writes under cameras/" portraits PUT /cameras/1/c.jpg --data-binary "@$work/portrait"
expect 403 "portraits deletes under cameras/" portraits DELETE /cameras/1/b.jpg
expect 403 "cameras reads under portraits/" cameras GET /portraits/a.jpg
expect 403 "cameras deletes under portraits/" cameras DELETE /portraits/a.jpg
expect 403 "portraits writes under portraitsx/" portraits PUT /portraitsx/a.jpg --data-binary "@$work/portrait"
expect 403 "portraits writes at the bucket root" portraits PUT /a.jpg --data-binary "@$work/portrait"
expect 403 "portraits lists cameras/" portraits GET "/?list-type=2&prefix=cameras/"
expect 403 "portraits lists the whole bucket" portraits GET "/?list-type=2"
expect 403 "portraits copies a camera object into portraits/" portraits PUT /portraits/copy.jpg \
  -H "x-amz-copy-source: /$bucket/cameras/1/b.jpg"
expect 403 "portraits starts a multipart upload under cameras/" portraits POST "/cameras/1/m.jpg?uploads"
expect 403 "portraits sets the bucket policy" portraits PUT "/?policy" --data-binary "@$work/bucket-policy"
expect 403 "portraits deletes the bucket" portraits DELETE /
expect 200 "cameras' object survived" root GET /cameras/1/b.jpg
expect 200 "portraits' object survived" root GET /portraits/a.jpg

if [ "$failures" -gt 0 ]; then
  printf 'rustfs-prefix-policy-test: %d request(s) were not answered as the policy says; RustFS does not enforce this prefix policy\n' "$failures"
  exit 1
fi
printf 'rustfs-prefix-policy-test: RustFS enforced every prefix rule\n'
