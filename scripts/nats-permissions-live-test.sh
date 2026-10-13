#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: ARGUS_NATS_SERVER_BIN=/path/to/nats-server scripts/nats-permissions-live-test.sh <nats-permissions-live-test>

Provisions a throwaway broker the way provision-host.sh provisions the stack's
(one nkey per row of argus-deploy/nats-permissions.conf plus the shared user),
starts it on a free loopback port that is never 4222, with JetStream in a
temporary directory, runs the given test binary against it and removes it all.
Exits 77 (skipped) when ARGUS_NATS_SERVER_BIN is not set.
USAGE
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
test_bin="$1"
server="${ARGUS_NATS_SERVER_BIN:-}"
if [ -z "$server" ]; then
  printf 'nats-permissions-live-test: skipped, ARGUS_NATS_SERVER_BIN is not set\n'
  exit 77
fi
[ -x "$server" ] || { printf 'nats-permissions-live-test: %s is not executable\n' "$server" >&2; exit 2; }
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
[ -n "$port" ] || { printf 'nats-permissions-live-test: no free port\n' >&2; exit 1; }

mkdir -p "$work/seeds" "$work/js"
chmod 700 "$work/seeds"
cp "$ROOT/argus-deploy/nats-permissions.conf" "$work/"
legacy="$(openssl rand -hex 16)"
printf '[nats]\npassword = "%s"\n' "$legacy" > "$work/legacy.toml"
nats_auth_conf "$work/nats-permissions.conf" "$work/seeds" "$work/legacy.toml" > "$work/auth.conf"
for seed in "$work"/seeds/*.seed; do
  nkey_public_of "$seed" > "${seed%.seed}.pub"
done
printf 'listen: "127.0.0.1:%s"\n\njetstream {\n  store_dir: "%s"\n}\n\ninclude ./auth.conf\n' \
  "$port" "$work/js" > "$work/nats.conf"

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
  printf 'nats-permissions-live-test: the broker did not start\n' >&2
  exit 1
fi

ARGUS_NATS_PERMISSIONS_URL="nats://127.0.0.1:$port" \
ARGUS_NATS_PERMISSIONS_SEEDS="$work/seeds" \
ARGUS_NATS_PERMISSIONS_LEGACY="$legacy" \
  "$test_bin"
