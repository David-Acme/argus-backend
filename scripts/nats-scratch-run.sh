#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: ARGUS_NATS_SERVER_BIN=/path/to/nats-server scripts/nats-scratch-run.sh <test-binary> [args...]

Runs a test binary whose live cases need a NATS broker. With a usable
nats-server binary (ARGUS_NATS_SERVER_BIN set and naming an executable file) it
starts a throwaway JetStream broker with no users on a random loopback port that
is never 4222, with its store in a temporary directory, runs the test with
ARGUS_NATS_URL and ARGUS_TEST_NATS_URL both set to that broker (any inherited
value of either is replaced), and removes the broker and its directory on exit.
It never uses a running broker or a nats-server on PATH.

Without a usable binary:
  CI=true   the run fails (exit 1): CI must exercise the live cases.
  otherwise the test runs without a broker, with ARGUS_NATS_URL and
            ARGUS_TEST_NATS_URL unset, so its live cases skip themselves and
            nothing can reach an inherited broker; a failure is returned as it
            is, and a pass exits 77 (skipped), because the live cases did not run.

Exit status: the test's own status when it ran against the broker; 77 as above;
2 when the test binary is not executable or no binary is given; 1 when the
broker cannot start or no safe port is free.
USAGE
}

[ "$#" -ge 1 ] || { usage >&2; exit 2; }
test_bin="$1"
shift

source "$ROOT/scripts/lib/nats-server-binary.sh"

if ! nats_server_binary_find; then
  nats_server_binary_fail_in_ci nats-scratch-run
  [ -x "$test_bin" ] || { printf 'nats-scratch-run: %s is not executable\n' "$test_bin" >&2; exit 2; }
  status=0
  env -u ARGUS_NATS_URL -u ARGUS_TEST_NATS_URL "$test_bin" "$@" || status=$?
  [ "$status" -eq 0 ] || exit "$status"
  printf 'nats-scratch-run: skipped the live cases, %s; the rest passed without a broker\n' "$NATS_SERVER_MISSING"
  exit 77
fi
[ -x "$test_bin" ] || { printf 'nats-scratch-run: %s is not executable\n' "$test_bin" >&2; exit 2; }

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

nats_scratch_port nats-scratch-run
port="$NATS_SCRATCH_PORT"
mkdir -p "$work/js"
printf 'listen: "127.0.0.1:%s"\n\njetstream {\n  store_dir: "%s"\n}\n' "$port" "$work/js" > "$work/nats.conf"

"$NATS_SERVER_BIN" -c "$work/nats.conf" > "$work/server.log" 2>&1 &
pid=$!
nats_scratch_wait nats-scratch-run "$pid" "$port" "$work/server.log"

ARGUS_NATS_URL="nats://127.0.0.1:$port" \
ARGUS_TEST_NATS_URL="nats://127.0.0.1:$port" \
  "$test_bin" "$@"
