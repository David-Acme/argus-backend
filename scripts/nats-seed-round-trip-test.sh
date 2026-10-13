#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/nats-seed-round-trip-test.sh <nats-seed-round-trip-test>

Issues an nkey seed with scripts/lib/nats-users.sh, starts an isolated
nats-server on a free loopback port that is never 4222, configured with that
seed's public key as its only user (publish and subscribe allowed on
argus.roundtrip.v1.allowed alone), and runs the given test binary against it:
the client connects through natsOptions_SetNKeyFromSeed, publishes inside its
allow-list, is refused outside it, and a second seed presented with the same
public key is refused at connect.

The nats-server binary is ARGUS_NATS_SERVER_BIN alone, never one on PATH.
Without a usable one (not set, empty or naming no executable file) the test
exits 77, which ctest reports as skipped, or fails with exit 1 when CI=true:
the decision and the CI rule are scripts/lib/nats-server-binary.sh's, shared
with scripts/nats-scratch-run.sh and scripts/nats-permissions-live-test.sh.
USAGE
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
test_bin="$1"

source "$ROOT/scripts/lib/nats-server-binary.sh"
nats_server_binary_or_exit nats-seed-round-trip-test
server="$NATS_SERVER_BIN"
[ -x "$test_bin" ] || { printf 'nats-seed-round-trip-test: %s is not executable\n' "$test_bin" >&2; exit 2; }

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

nats_scratch_port nats-seed-round-trip-test
port="$NATS_SCRATCH_PORT"

nkey_issue "$work/client.seed" || { printf 'nats-seed-round-trip-test: the seed could not be issued\n' >&2; exit 1; }
nkey_issue "$work/other.seed" || { printf 'nats-seed-round-trip-test: the second seed could not be issued\n' >&2; exit 1; }
public="$(nkey_public_of "$work/client.seed")"
cat > "$work/nats.conf" <<CONF
listen: "127.0.0.1:$port"

authorization {
  users: [
    {nkey: "$public", permissions: {publish: {allow: ["argus.roundtrip.v1.allowed"]}, subscribe: {allow: ["argus.roundtrip.v1.allowed"]}}}
  ]
}
CONF

"$server" -c "$work/nats.conf" > "$work/server.log" 2>&1 &
pid=$!
nats_scratch_wait nats-seed-round-trip-test "$pid" "$port" "$work/server.log"

ARGUS_NATS_ROUNDTRIP_URL="nats://127.0.0.1:$port" \
ARGUS_NATS_ROUNDTRIP_NKEY="$public" \
ARGUS_NATS_ROUNDTRIP_SEED="$work/client.seed" \
ARGUS_NATS_ROUNDTRIP_OTHER_SEED="$work/other.seed" \
  "$test_bin"
