#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="$ROOT/argus-deploy/docker-compose.yml"
CONFIG="$ROOT/argus-deploy/config.llm.toml.example"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

fail() {
  printf 'deploy-mounts-test: %s\n' "$*" >&2
  exit 1
}

service_block() {
  awk -v name="$2" '
    $0 == "  " name ":" { on = 1; next }
    on && /^  [A-Za-z0-9_-]+:/ { exit }
    on { print }
  ' "$1"
}

missing_mounts() {
  local compose="$1" config="$2" block dir referenced
  block="$(service_block "$compose" argus-llm)"
  [ -n "$block" ] || { echo argus-llm; return; }
  referenced="$(grep -Eo '"models/[a-z0-9_-]+/' "$config" | sed -E 's#"models/([a-z0-9_-]+)/#\1#' | sort -u)"
  for dir in $(printf '%s\n' $referenced intent | sort -u); do
    grep -Eq "^[[:space:]]+target: /opt/argus/models/$dir\$" <<<"$block" || echo "$dir"
  done
}

[ -z "$(missing_mounts "$COMPOSE" "$CONFIG")" ] \
  || fail "argus-llm does not mount: $(missing_mounts "$COMPOSE" "$CONFIG" | tr '\n' ' ')"

grep -Eq '^model_file = "models/intent/intent.bin"$' "$CONFIG" \
  || fail "$CONFIG does not name the intent model"

grep -v 'target: /opt/argus/models/intent' "$COMPOSE" > "$TEST_TMP/compose.yml"
[ "$(missing_mounts "$TEST_TMP/compose.yml" "$CONFIG")" = "intent" ] \
  || fail "a compose file without the intent mount was not reported"

printf 'deploy-mounts-test: ok\n'
