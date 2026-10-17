#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: ARGUS_NATS_SERVER_BIN=/path/to/nats-server scripts/nats-permissions-live-test.sh <nats-permissions-live-test>

Provisions a throwaway broker the way provision-host.sh provisions the stack's
(one nkey per row of argus-deploy/nats-permissions.conf) plus one unrestricted
probe user of its own, with a fresh password each run, so the cases that need a
client outside every row have one that no config and no image ever carries. It
starts the broker on a free loopback port that is never 4222, with JetStream in
a temporary directory, runs the given test binary against it and removes it all.
Without a usable nats-server (ARGUS_NATS_SERVER_BIN not set, empty or naming no
executable file) it exits 77 (skipped), or fails with exit 1 when CI=true: the
decision and the CI rule are scripts/lib/nats-server-binary.sh's, shared with
scripts/nats-scratch-run.sh and scripts/nats-seed-round-trip-test.sh. It exits
2 when the test binary is not executable, and never falls back to a running
broker, to ARGUS_NATS_URL or to a nats-server on PATH.
USAGE
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
test_bin="$1"

source "$ROOT/scripts/lib/nats-server-binary.sh"
nats_server_binary_or_exit nats-permissions-live-test
server="$NATS_SERVER_BIN"
[ -x "$test_bin" ] || { printf 'nats-permissions-live-test: %s is not executable\n' "$test_bin" >&2; exit 2; }

source "$ROOT/scripts/lib/nats-users.sh"

work="$(mktemp -d)"
pid=""
cleanup() {
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

nats_scratch_port nats-permissions-live-test
port="$NATS_SCRATCH_PORT"

mkdir -p "$work/seeds" "$work/js"
chmod 700 "$work/seeds"
cp "$ROOT/argus-deploy/nats-permissions.conf" "$work/"
probe="$(openssl rand -hex 16)"
nats_auth_conf "$work/nats-permissions.conf" "$work/seeds" > "$work/users.conf"
awk -v line="    {user: \"probe\", password: \"$probe\"}" '
  /^  \]$/ { print line }
  { print }' "$work/users.conf" > "$work/auth.conf"
for seed in "$work"/seeds/*.seed; do
  nkey_public_of "$seed" > "${seed%.seed}.pub"
done
printf 'listen: "127.0.0.1:%s"\n\njetstream {\n  store_dir: "%s"\n}\n\ninclude ./auth.conf\n' \
  "$port" "$work/js" > "$work/nats.conf"

"$server" -c "$work/nats.conf" > "$work/server.log" 2>&1 &
pid=$!
nats_scratch_wait nats-permissions-live-test "$pid" "$port" "$work/server.log"

ARGUS_NATS_PERMISSIONS_URL="nats://127.0.0.1:$port" \
ARGUS_NATS_PERMISSIONS_SEEDS="$work/seeds" \
ARGUS_NATS_PERMISSIONS_PROBE="$probe" \
  "$test_bin"
