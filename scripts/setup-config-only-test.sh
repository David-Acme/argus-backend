#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/setup-config-only-test.sh

Runs scripts/setup.sh --config-only in a scratch root that holds only the
scripts and the services' config.toml.example files, with mock cmake, conan,
docker and apt-get on PATH. Checks that it writes every per-service config at
mode 0600, the instance and internal PKI, exits 0 without a privacy prompt,
never calls a build, toolchain or package command, and runs again unchanged.
Checks that the full setup still stops at the privacy notice without consent.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

TREE="$TEST_TMP/tree"
CALLS="$TEST_TMP/calls.log"
MOCK_BIN="$TEST_TMP/bin"
mkdir -p "$TREE/services" "$MOCK_BIN"

cp -r "$ROOT/scripts" "$TREE/"
examples=0
for dir in "$ROOT"/services/*/; do
  name="$(basename "$dir")"
  if [ -f "$dir/config.toml.example" ]; then
    mkdir -p "$TREE/services/$name"
    cp "$dir/config.toml.example" "$TREE/services/$name/"
    examples=$((examples + 1))
  fi
done
test "$examples" -ge 14

for tool in cmake conan docker apt-get; do
  printf '%s\n' \
    '#!/usr/bin/env bash' \
    "printf '%s %s\n' '$tool' \"\$*\" >> '$CALLS'" \
    'exit 99' > "$MOCK_BIN/$tool"
  chmod +x "$MOCK_BIN/$tool"
done

run_setup() {
  env PATH="$MOCK_BIN:$PATH" bash "$TREE/scripts/setup.sh" "$@" < /dev/null
}

output="$(cd "$TREE" && run_setup --config-only 2>&1)" || {
  printf '%s\n' "$output" >&2
  echo "setup.sh --config-only failed" >&2
  exit 1
}

if [ -s "$CALLS" ] 2>/dev/null; then
  cat "$CALLS" >&2
  echo "setup.sh --config-only called a build, toolchain or package command" >&2
  exit 1
fi
if grep -qi "privacy notice" <<<"$output"; then
  echo "setup.sh --config-only showed the privacy notice" >&2
  exit 1
fi

configs=0
for example in "$TREE"/services/*/config.toml.example; do
  dir="$(dirname "$example")"
  test -f "$dir/config.toml"
  test "$(stat -c '%a' "$dir/config.toml")" = 600
  configs=$((configs + 1))
done
test "$configs" -eq "$examples"

test -f "$TREE/certs/ca.pem"
test -f "$TREE/certs/ca.key"
test -f "$TREE/argus-deploy/data/certs/internal/ca.pem"
test -f "$TREE/argus-deploy/data/certs/internal/ca.key"
for service in auth identity sync guard; do
  test -f "$TREE/argus-deploy/data/certs/internal/$service/cert.pem"
done

run_setup --config-only > "$TEST_TMP/again.out" 2>&1
grep -q "Configs and PKI are ready" "$TEST_TMP/again.out"

if (cd "$TREE" && run_setup > "$TEST_TMP/full.out" 2>&1); then
  echo "the full setup ran without privacy consent" >&2
  exit 1
fi
grep -qi "privacy" "$TEST_TMP/full.out"
if [ -s "$CALLS" ]; then
  cat "$CALLS" >&2
  echo "the full setup without consent called a build, toolchain or package command" >&2
  exit 1
fi

echo "setup config-only test passed"
