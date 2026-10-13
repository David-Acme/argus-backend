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

The nats-server binary is ARGUS_NATS_SERVER_BIN, or nats-server on PATH.
Without one the test exits 77, which ctest reports as skipped.
USAGE
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
test_bin="$1"
server="${ARGUS_NATS_SERVER_BIN:-$(command -v nats-server || true)}"
if [ -z "$server" ] || [ ! -x "$server" ]; then
  printf 'nats-seed-round-trip-test: skipped, no nats-server binary (set ARGUS_NATS_SERVER_BIN or put nats-server on PATH)\n'
  exit 77
fi
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

port_free() {
  ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

port=""
for _ in $(seq 1 50); do
  candidate=$(( 20000 + RANDOM % 40000 ))
  [ "$candidate" -ne 4222 ] || continue
  if port_free "$candidate"; then
    port="$candidate"
    break
  fi
done
[ -n "$port" ] || { printf 'nats-seed-round-trip-test: no free port\n' >&2; exit 1; }

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
ready=""
for _ in $(seq 1 200); do
  if ! port_free "$port"; then
    ready=1
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.05
done
if [ -z "$ready" ]; then
  cat "$work/server.log" >&2
  printf 'nats-seed-round-trip-test: the broker did not start\n' >&2
  exit 1
fi

ARGUS_NATS_ROUNDTRIP_URL="nats://127.0.0.1:$port" \
ARGUS_NATS_ROUNDTRIP_NKEY="$public" \
ARGUS_NATS_ROUNDTRIP_SEED="$work/client.seed" \
ARGUS_NATS_ROUNDTRIP_OTHER_SEED="$work/other.seed" \
  "$test_bin"
