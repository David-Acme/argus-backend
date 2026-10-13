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

zone_script() {
  local etc="$1"
  printf 'source %q\n' "$ROOT/scripts/lib/common.sh"
  awk '
    /^(zone_from_path|host_timezone)\(\) \{$/ { on = 1 }
    on { print }
    on && /^}$/ { on = 0 }
  ' "$ROOT/scripts/provision-host.sh" | sed -e "s#/etc/localtime#$etc/localtime#g" -e "s#/etc/timezone#$etc/timezone#g"
  printf 'host_timezone\n'
}

mkdir -p "$TEST_TMP/no-zone" "$TEST_TMP/linked-zone"
zone_script "$TEST_TMP/no-zone" > "$TEST_TMP/no-zone.sh"
env -u TZ bash "$TEST_TMP/no-zone.sh" > "$TEST_TMP/no-zone.out" 2> "$TEST_TMP/no-zone.err"
[ "$(cat "$TEST_TMP/no-zone.out")" = "UTC" ] \
  || fail "host_timezone without a zone source printed '$(cat "$TEST_TMP/no-zone.out")' instead of UTC"
grep -q 'ARGUS_TZ falls back to UTC' "$TEST_TMP/no-zone.err" \
  || fail "host_timezone without a zone source did not warn on stderr"

ln -s /usr/share/zoneinfo/America/Lima "$TEST_TMP/linked-zone/localtime"
zone_script "$TEST_TMP/linked-zone" > "$TEST_TMP/linked-zone.sh"
env -u TZ bash "$TEST_TMP/linked-zone.sh" > "$TEST_TMP/linked-zone.out" 2> "$TEST_TMP/linked-zone.err"
[ "$(cat "$TEST_TMP/linked-zone.out")" = "America/Lima" ] \
  || fail "host_timezone did not read the zone /etc/localtime links to"
[ ! -s "$TEST_TMP/linked-zone.err" ] || fail "host_timezone warned although the host zone was readable"

runtime_packages() {
  awk '
    / AS runtime$/ { stage = 1; next }
    !stage { next }
    {
      for (i = 1; i <= NF; i++) {
        if ($i == "install" && i > 1 && $(i - 1) == "apt-get") { listing = 1; continue }
        if ($i == "&&") listing = 0
        if (listing && $i != "\\" && $i !~ /^-/) print $i
      }
    }
  ' "$1"
}

images_without_tzdata() {
  local dockerfile packages
  for dockerfile in "$@"; do
    packages="$(runtime_packages "$dockerfile")"
    grep -qx tzdata <<<"$packages" || echo "$dockerfile"
  done
}

dockerfiles=("$ROOT"/services/*/Dockerfile)
[ "${#dockerfiles[@]}" -ge 14 ] || fail "the service Dockerfiles are too few to be the stack"
[ -z "$(images_without_tzdata "${dockerfiles[@]}")" ] \
  || fail "runtime images without tzdata: $(images_without_tzdata "${dockerfiles[@]}" | tr '\n' ' ')"

sed -E -e 's/[[:space:]]tzdata([[:space:]])/\1/' -e 's/build-essential /build-essential tzdata /' \
  "$ROOT/services/auth/Dockerfile" > "$TEST_TMP/Dockerfile"
grep -q 'build-essential tzdata ' "$TEST_TMP/Dockerfile" \
  || fail "the negative Dockerfile lost the build-stage tzdata it must ignore"
[ "$(images_without_tzdata "$TEST_TMP/Dockerfile")" = "$TEST_TMP/Dockerfile" ] \
  || fail "a runtime image without tzdata was not reported"

printf 'deploy-mounts-test: ok\n'
