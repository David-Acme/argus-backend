#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

CALL_LOG="$TEST_TMP/calls.log"
MOCK_BIN="$TEST_TMP/bin"
mkdir -p "$MOCK_BIN"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s %s\n" "$(basename "$0")" "$*" >> "$ARGUS_BUILD_ALL_TEST_LOG"' \
  > "$MOCK_BIN/tool"
chmod +x "$MOCK_BIN/tool"
ln -s tool "$MOCK_BIN/conan"
ln -s tool "$MOCK_BIN/cmake"
ln -s tool "$MOCK_BIN/ctest"

run_build_all() {
  : > "$CALL_LOG"
  PATH="$MOCK_BIN:$PATH" ARGUS_BUILD_ALL_TEST_LOG="$CALL_LOG" \
    "$ROOT/scripts/build-all.sh" "$@"
}

run_build_all dev --only argus-common --install-only
grep -q '^conan install ' "$CALL_LOG"
if grep -Eq '^(cmake|ctest) ' "$CALL_LOG"; then
  echo "--install-only invoked a build or test command" >&2
  exit 1
fi

run_build_all prod --only argus-common --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 2
if grep -q '^ctest ' "$CALL_LOG"; then
  echo "--no-tests invoked ctest" >&2
  exit 1
fi

run_build_all dev --only argus-identity --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 3
grep -q '^cmake --build --preset dev -j 8 --target argus-migrate-identity$' "$CALL_LOG"

run_build_all prod --only argus-camera --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 3
grep -q '^cmake --build --preset prod -j 8 --target argus-migrate-camera argus-vulkan-probe$' "$CALL_LOG"

if run_build_all dev --only does-not-exist; then
  echo "unknown --only project unexpectedly succeeded" >&2
  exit 1
fi

grep -Fq '"$ROOT/scripts/build-all.sh" "$PROFILE" --install-only' \
  "$ROOT/scripts/setup.sh"
grep -Fq '"$ROOT/scripts/build-all.sh" "$PROFILE"' \
  "$ROOT/scripts/setup.sh"

grep -Fq 'git rev-parse --is-inside-work-tree' "$ROOT/scripts/setup.sh"
if grep -Fq '[ ! -d ".git" ]' "$ROOT/scripts/setup.sh"; then
  echo "setup still rejects linked Git worktrees" >&2
  exit 1
fi

echo "build-all tests passed"
