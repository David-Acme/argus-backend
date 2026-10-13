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

nats_clients() {
  local template name
  for template in "$ROOT"/argus-deploy/config.*.toml.example; do
    grep -qx '\[nats\]' "$template" || continue
    name="${template##*/config.}"
    printf '%s\n' "${name%.toml.example}"
  done
}

bind_mounts() {
  service_block "$1" "$2" | awk '
    /^      - type: bind$/ { source = ""; target = ""; ro = 0; next }
    /^        source: / { source = $2 }
    /^        target: / { target = $2 }
    /^        read_only: true$/ { ro = 1 }
    /^          create_host_path: false$/ { print source " " target " " ro }
  '
}

misplaced_seeds() {
  local compose="$1" clients name service expected
  clients="$(nats_clients)"
  for name in $(service_names "$compose"); do
    service="${name#argus-}"
    expected=""
    if grep -qx "$service" <<<"$clients"; then
      expected="\${ARGUS_DATA_DIR:-./data}/nats/seeds/$service.seed /run/secrets/nats.seed 1"
    fi
    [ "$(bind_mounts "$compose" "$name" | grep '/nats/seeds/' || true)" = "$expected" ] || echo "$name"
  done
  for service in $clients; do
    [ -n "$(service_block "$compose" "argus-$service")" ] || echo "argus-$service"
  done
}

[ "$(nats_clients | wc -l)" -ge 10 ] || fail "the NATS clients are too few to be the stack"
[ -z "$(misplaced_seeds "$COMPOSE")" ] \
  || fail "services whose NATS seed mount is missing or not their own: $(misplaced_seeds "$COMPOSE" | tr '\n' ' ')"
grep -qx '\${ARGUS_DATA_DIR:-./data}/nats/auth.conf /etc/nats/auth.conf 1' <<<"$(bind_mounts "$COMPOSE" nats)" \
  || fail "nats does not mount the provisioned auth.conf read-only"
grep -qx './nats-permissions.conf /etc/nats/nats-permissions.conf 1' <<<"$(bind_mounts "$COMPOSE" nats)" \
  || fail "nats does not mount the checked-in permission config beside auth.conf"

sed 's#nats/seeds/camera.seed#nats/seeds/guard.seed#' "$COMPOSE" > "$TEST_TMP/compose-borrowed-seed.yml"
[ "$(misplaced_seeds "$TEST_TMP/compose-borrowed-seed.yml")" = "argus-camera" ] \
  || fail "a service mounting another service's seed was not reported"
grep -v 'nats/seeds/sync.seed' "$COMPOSE" > "$TEST_TMP/compose-no-seed.yml"
[ "$(misplaced_seeds "$TEST_TMP/compose-no-seed.yml")" = "argus-sync" ] \
  || fail "a NATS client without its seed mount was not reported"

source "$ROOT/scripts/lib/nats-users.sh"
documented_seed="SUACSSL3UAHUDXKFSNVUZRF5UHPMWZ6BFDTJ7M6USDXIEDNPPQYYYCU3VY"
documented_hex="$(nkey_seed_hex "$documented_seed")" || fail "the documented NATS seed does not decode"
[ "$(nkey_public_text "$(nkey_public_hex "$documented_hex")")" = "UDXU4RCSJNZOIQHZNWXHXORDPRTGNJAHAHFRGZNEEJCPQTT2M7NLCNF4" ] \
  || fail "the documented NATS seed does not derive its documented public key"
[ "$(nkey_seed_text "$documented_hex")" = "$documented_seed" ] \
  || fail "the documented NATS seed does not encode back to itself"
! nkey_seed_hex "SUAMK2FG4MI6UE3ACF3FK3OIQBCEIEZV7NSWFFEW63UXMRLFM2XLAXK4AA" >/dev/null \
  || fail "a NATS seed with a wrong checksum was accepted"

permissions="$ROOT/argus-deploy/nats-permissions.conf"
mkdir -p "$TEST_TMP/seeds"
nats_auth_conf "$permissions" "$TEST_TMP/seeds" legacy > "$TEST_TMP/auth.conf" \
  || fail "the NATS users could not be rendered"
nats_auth_conf "$permissions" "$TEST_TMP/seeds" legacy > "$TEST_TMP/auth-again.conf" \
  || fail "the NATS users could not be rendered a second time"
cmp -s "$TEST_TMP/auth.conf" "$TEST_TMP/auth-again.conf" || fail "a second provisioning run changed the NATS users"
grep -qx 'include ./nats-permissions.conf' "$TEST_TMP/auth.conf" || fail "auth.conf does not include the permission config"
grep -qx '    {user: "argus", password: "legacy"}' "$TEST_TMP/auth.conf" || fail "the shared NATS user is gone before every service switched"
! grep -Eq 'SU[A-Z2-7]{56}' "$TEST_TMP/auth.conf" || fail "auth.conf carries a seed"
for variable in $(sed -n 's/^\(ARGUS_NATS_[A-Z0-9_]*\): {$/\1/p' "$permissions"); do
  [ "$(grep -Ec "^    \{nkey: \"U[A-Z2-7]{55}\", permissions: \\\$$variable\}$" "$TEST_TMP/auth.conf")" = 1 ] \
    || fail "auth.conf does not give $variable to exactly one nkey user"
done
[ "$(grep -c '{nkey: ' "$TEST_TMP/auth.conf")" = "$(grep -c '^ARGUS_NATS_' "$permissions")" ] \
  || fail "auth.conf names an nkey user the permission config does not"
[ -z "$(find "$TEST_TMP/seeds" -name '*.seed' ! -perm 600)" ] || fail "a NATS seed is not 0600"
for seed in "$TEST_TMP"/seeds/*.seed; do
  [ "$(grep -c "\"$(nkey_public_of "$seed")\"" "$TEST_TMP/auth.conf")" = 1 ] \
    || fail "the seed $seed does not match its user in auth.conf"
done
printf 'SUAMK2FG4MI6UE3ACF3FK3OIQBCEIEZV7NSWFFEW63UXMRLFM2XLAXK4AA\n' > "$TEST_TMP/seeds/sync.seed"
! nats_auth_conf "$permissions" "$TEST_TMP/seeds" legacy >/dev/null 2>&1 \
  || fail "a corrupt NATS seed was accepted"

grep -Eq '^  if ! nats_auth_conf "\$permissions" "\$seeds" "\$password" > "\$temp"; then$' "$ROOT/scripts/provision-host.sh" \
  || fail "provision-host.sh does not render the NATS users from the permission config"

if [ -n "${ARGUS_NATS_SERVER_BIN:-}" ]; then
  mkdir -p "$TEST_TMP/broker/js"
  cp "$permissions" "$TEST_TMP/broker/"
  nats_auth_conf "$permissions" "$TEST_TMP/broker" legacy > "$TEST_TMP/broker/auth.conf"
  sed -e "s#\"/data\"#\"$TEST_TMP/broker/js\"#" "$ROOT/argus-deploy/nats.conf" > "$TEST_TMP/broker/nats.conf"
  "$ARGUS_NATS_SERVER_BIN" -c "$TEST_TMP/broker/nats.conf" -t > "$TEST_TMP/broker/check.out" 2>&1 \
    || fail "nats-server refuses the provisioned configuration: $(tail -n 1 "$TEST_TMP/broker/check.out")"
  printf 'deploy-mounts-test: nats-server accepts the provisioned configuration\n'
fi

printf 'deploy-mounts-test: ok\n'
