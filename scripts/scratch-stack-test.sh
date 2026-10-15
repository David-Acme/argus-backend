#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/scratch-stack-test.sh

Checks the guards of scripts/scratch-stack.sh on a fake tree, without starting
anything: --check passes on a complete tree and changes nothing (the NATS URL
and the stack directory stay as they were); a Git work tree, a NATS port of the
production broker or of the native services, a missing binary, a real models
directory, a missing go2rtc and an unknown stack all refuse with the reason.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

SERVICES=(auth identity notification sync camera guard productivity settings llm stt tts vlm)

fake_tree() {
  local tree="$1" svc
  mkdir -p "$tree/third_party/go2rtc"
  for svc in "${SERVICES[@]}"; do
    mkdir -p "$tree/services/$svc/build/dev"
    printf 'listen = "nats://127.0.0.1:4222"\n[nats]\nurl = "nats://127.0.0.1:4222"\n' \
      > "$tree/services/$svc/config.toml"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$tree/services/$svc/build/dev/argus-$svc"
    chmod +x "$tree/services/$svc/build/dev/argus-$svc"
  done
  printf '#!/usr/bin/env bash\nexit 0\n' > "$tree/nats-server"
  chmod +x "$tree/nats-server"
}

expect_refusal() {
  local name="$1" reason="$2"
  shift 2
  local output code=0
  output="$("$ROOT/scripts/scratch-stack.sh" "$@" 2>&1)" || code=$?
  if [ "$code" -eq 0 ]; then
    echo "$name: accepted" >&2
    exit 1
  fi
  if ! grep -Fq "$reason" <<<"$output"; then
    echo "$name: refused without '$reason':" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
}

TREE="$TEST_TMP/tree"
STACK="$TEST_TMP/stack"
fake_tree "$TREE"
export ARGUS_NATS_SERVER_BIN="$TREE/nats-server"

mkdir -p "$TEST_TMP/models-src"
output="$("$ROOT/scripts/scratch-stack.sh" --check "$TREE" "$STACK" --with-ai --models "$TEST_TMP/models-src" 2>&1)"
grep -Fq "check passed: nothing changed" <<<"$output"
grep -Fq "127.0.0.1:4222" "$TREE/services/auth/config.toml"
test ! -e "$STACK"

output="$("$ROOT/scripts/scratch-stack.sh" --check "$TREE" "$STACK" --with-ai --models "$TEST_TMP/models-src" 2>&1)"
grep -Fq "check passed: nothing changed" <<<"$output"
test ! -e "$TREE/models"

mkdir -p "$TEST_TMP/git-tree"
fake_tree "$TEST_TMP/git-tree"
mkdir "$TEST_TMP/git-tree/.git"
expect_refusal "git work tree" "is a Git work tree" "$TEST_TMP/git-tree" "$STACK" --check

expect_refusal "production NATS port" "belongs to the production broker" \
  "$TREE" "$STACK" --nats-port 4222 --check
expect_refusal "native service port" "belongs to the production broker or the native services" \
  "$TREE" "$STACK" --nats-port 7030 --check

mv "$TREE/services/sync/build/dev/argus-sync" "$TREE/services/sync/build/dev/argus-sync.off"
expect_refusal "missing binary" "missing binary $TREE/services/sync/build/dev/argus-sync" \
  "$TREE" "$STACK" --check
mv "$TREE/services/sync/build/dev/argus-sync.off" "$TREE/services/sync/build/dev/argus-sync"

mkdir -p "$TEST_TMP/real-models-tree"
fake_tree "$TEST_TMP/real-models-tree"
mkdir "$TEST_TMP/real-models-tree/models"
expect_refusal "real models directory" "is a real directory; it is not replaced" \
  "$TEST_TMP/real-models-tree" "$STACK" --models "$TEST_TMP/models-src" --check

rm -rf "$TREE/third_party/go2rtc"
expect_refusal "missing go2rtc" "third_party/go2rtc is missing" \
  "$TREE" "$STACK" --models "$TEST_TMP/models-src" --check
mkdir -p "$TREE/third_party/go2rtc"

unset_output="$(env -u ARGUS_NATS_SERVER_BIN "$ROOT/scripts/scratch-stack.sh" "$TREE" "$STACK" --models "$TEST_TMP/models-src" --check 2>&1)" && {
  echo "unset broker binary: accepted" >&2
  exit 1
}
grep -Fq "ARGUS_NATS_SERVER_BIN must name an executable nats-server" <<<"$unset_output"

output="$(ARGUS_NATS_SERVER_BIN= "$ROOT/scripts/scratch-stack.sh" --down "$TREE" "$TEST_TMP/empty-stack" 2>&1)"
grep -Fq "scratch stack down" <<<"$output"

echo "scratch stack test passed"
