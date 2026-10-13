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

service_names() {
  awk '
    /^services:/ { on = 1; next }
    on && /^[A-Za-z]/ { exit }
    on && /^  [A-Za-z0-9_-]+:$/ { sub(/^  /, ""); sub(/:$/, ""); print }
  ' "$1"
}

services_without_zone() {
  local compose="$1" name block
  for name in $(service_names "$compose"); do
    block="$(service_block "$compose" "$name")"
    grep -Eq '^    <<: .*\*host-zone' <<<"$block" && continue
    grep -Eq '^      TZ: ' <<<"$block" && continue
    echo "$name"
  done
}

grep -Eq '^x-host-zone: &host-zone$' "$COMPOSE" \
  || fail "$COMPOSE does not define the x-host-zone anchor"
grep -Eq '^    TZ: "\$\{ARGUS_TZ:-UTC\}"$' "$COMPOSE" \
  || fail "the x-host-zone anchor does not set TZ from ARGUS_TZ"
[ "$(service_names "$COMPOSE" | wc -l)" -gt 20 ] || fail "the service list is too short to be the stack"
[ -z "$(services_without_zone "$COMPOSE")" ] \
  || fail "services without a host zone: $(services_without_zone "$COMPOSE" | tr '\n' ' ')"

sed -e 's/, \*host-zone\]/]/' -e 's/\[\*hardening, \*host-zone\]/*hardening/' "$COMPOSE" > "$TEST_TMP/compose-no-zone.yml"
unzoned="$(services_without_zone "$TEST_TMP/compose-no-zone.yml")"
grep -qx argus-llm <<<"$unzoned" \
  || fail "a compose file without the host zone was not reported"

grep -Eq '^  ensure_env_value "\$env_file" ARGUS_TZ "\$\(host_timezone\)"$' "$ROOT/scripts/provision-host.sh" \
  || fail "provision-host.sh does not write ARGUS_TZ from the host zone"

printf 'deploy-mounts-test: ok\n'
