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
  for dir in $(printf '%s\n' $referenced decide extract | sort -u); do
    grep -Eq "^[[:space:]]+target: /opt/argus/models/$dir\$" <<<"$block" || echo "$dir"
  done
}

[ -z "$(missing_mounts "$COMPOSE" "$CONFIG")" ] \
  || fail "argus-llm does not mount: $(missing_mounts "$COMPOSE" "$CONFIG" | tr '\n' ' ')"

grep -Eq '^bundle_dir = "models/decide/[A-Za-z0-9_.-]+"$' "$CONFIG" \
  || fail "$CONFIG does not name the decider bundle"
grep -Eq '^bundle_dir = "models/extract/[A-Za-z0-9_.-]+"$' "$CONFIG" \
  || fail "$CONFIG does not name the extractor bundle"
! grep -Eq '^\[intent\]|^engine = ' "$CONFIG" \
  || fail "$CONFIG still selects an engine"

grep -v 'target: /opt/argus/models/decide' "$COMPOSE" > "$TEST_TMP/compose.yml"
[ "$(missing_mounts "$TEST_TMP/compose.yml" "$CONFIG")" = "decide" ] \
  || fail "a compose file without the decider bundle mount was not reported"

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
    service="${service%-client}"
    expected=""
    if grep -qx "$service" <<<"$clients"; then
      expected="\${ARGUS_DATA_DIR:-./data}/nats/seeds/$service.seed /run/secrets/nats.seed 1"
    fi
    [ "$(bind_mounts "$compose" "$name" | grep '/nats/seeds/' || true)" = "$expected" ] || echo "$name"
  done
  for service in $clients; do
    [ -n "$(service_block "$compose" "argus-$service")" ] \
      || [ -n "$(service_block "$compose" "argus-$service-client")" ] || echo "argus-$service"
  done
}

published_ports() {
  service_block "$COMPOSE" "$1" | awk '/^    ports:$/ { on = 1; next } on && /^      - / { print; next } { on = 0 }'
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
grep -v 'nats/seeds/tunnel.seed' "$COMPOSE" > "$TEST_TMP/compose-no-tunnel-seed.yml"
[ "$(misplaced_seeds "$TEST_TMP/compose-no-tunnel-seed.yml")" = "argus-tunnel-client" ] \
  || fail "the tunnel client without its seed mount was not reported"

[ "$(published_ports nats)" = '      - "127.0.0.1:${NATS_CLIENT_PORT:-4222}:4222"' ] \
  || fail "nats publishes its client port other than on 127.0.0.1 alone: $(published_ports nats | tr '\n' ' ')"
nats_conf="$ROOT/argus-deploy/nats.conf"
[ "$(grep -c '^port: 4222$' "$nats_conf")" = 1 ] \
  && ! grep -Eq '^[[:space:]]*(listen|leafnodes|websocket|mqtt|gateway|cluster)[[:space:]:{]' "$nats_conf" \
  || fail "nats.conf opens a client listener besides port 4222"
grep -qx 'http: "127.0.0.1:8222"' "$nats_conf" || fail "the broker's monitor listens beyond its own loopback"
grep -qx 'include ./auth.conf' "$nats_conf" || fail "nats.conf does not take its users from auth.conf"
! grep -q 'no_auth_user' "$nats_conf" || fail "nats.conf admits a client without credentials"
tunnel_client="$(service_block "$COMPOSE" argus-tunnel-client)"
grep -qx '    network_mode: host' <<<"$tunnel_client" || fail "argus-tunnel-client is no longer host-networked"
grep -A1 -x '      nats:' <<<"$tunnel_client" | grep -qx '        condition: service_healthy' \
  || fail "argus-tunnel-client does not wait for a healthy broker"
sed 's#"127.0.0.1:${NATS_CLIENT_PORT:-4222}:4222"#"${NATS_CLIENT_PORT:-4222}:4222"#' "$COMPOSE" > "$TEST_TMP/compose-open-nats.yml"
[ "$(COMPOSE="$TEST_TMP/compose-open-nats.yml" published_ports nats)" != "$(published_ports nats)" ] \
  || fail "a nats publish on every interface looked like the loopback one"

source "$ROOT/scripts/lib/nats-users.sh"
documented_seed="SUACSSL3UAHUDXKFSNVUZRF5UHPMWZ6BFDTJ7M6USDXIEDNPPQYYYCU3VY"
printf '%s\n' "$documented_seed" > "$TEST_TMP/documented.seed"
[ "$(nkey_public_of "$TEST_TMP/documented.seed")" = "UDXU4RCSJNZOIQHZNWXHXORDPRTGNJAHAHFRGZNEEJCPQTT2M7NLCNF4" ] \
  || fail "the documented NATS seed does not derive its documented public key"
[ "$(nkey_seed_payload < "$TEST_TMP/documented.seed" | nkey_encode "$NKEY_USER_SEED_PREFIX")" = "$documented_seed" ] \
  || fail "the documented NATS seed does not encode back to itself"
printf 'SUAMK2FG4MI6UE3ACF3FK3OIQBCEIEZV7NSWFFEW63UXMRLFM2XLAXK4AA\n' > "$TEST_TMP/corrupt.seed"
! nkey_public_of "$TEST_TMP/corrupt.seed" >/dev/null 2>&1 \
  || fail "a NATS seed with a wrong checksum was accepted"
[ -z "$(nkey_public_of "$TEST_TMP/corrupt.seed" 2>/dev/null)" ] \
  || fail "a NATS seed with a wrong checksum still printed a public key"

permissions="$ROOT/argus-deploy/nats-permissions.conf"
mkdir -p "$TEST_TMP/seeds"
legacy="$TEST_TMP/legacy.toml"
printf '[nats]\npassword = "legacy"\n' > "$legacy"
nats_auth_conf "$permissions" "$TEST_TMP/seeds" "$legacy" > "$TEST_TMP/auth.conf" \
  || fail "the NATS users could not be rendered"
nats_auth_conf "$permissions" "$TEST_TMP/seeds" "$legacy" > "$TEST_TMP/auth-again.conf" \
  || fail "the NATS users could not be rendered a second time"
cmp -s "$TEST_TMP/auth.conf" "$TEST_TMP/auth-again.conf" || fail "a second provisioning run changed the NATS users"
grep -qx 'include ./nats-permissions.conf' "$TEST_TMP/auth.conf" || fail "auth.conf does not include the permission config"
grep -qx 'authorization {' "$TEST_TMP/auth.conf" && ! grep -q 'no_auth_user' "$TEST_TMP/auth.conf" \
  || fail "auth.conf admits a client without credentials"
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
printf '[nats]\npassword = "not-alphanumeric"\n' > "$TEST_TMP/bad-legacy.toml"
! nats_auth_conf "$permissions" "$TEST_TMP/seeds" "$TEST_TMP/bad-legacy.toml" >/dev/null 2>&1 \
  || fail "a NATS password that is not alphanumeric was rendered"
cp "$TEST_TMP/corrupt.seed" "$TEST_TMP/seeds/sync.seed"
! nats_auth_conf "$permissions" "$TEST_TMP/seeds" "$legacy" >/dev/null 2>&1 \
  || fail "a corrupt NATS seed was accepted"

grep -Eq '^  if ! nats_auth_conf "\$permissions" "\$seeds" "\$config" > "\$temp"; then$' "$ROOT/scripts/provision-host.sh" \
  || fail "provision-host.sh does not render the NATS users from the permission config"

source "$ROOT/scripts/lib/common.sh"
for service in $(nats_clients); do
  template="$ROOT/argus-deploy/config.$service.toml.example"
  [ "$(toml_value "$template" nats seed_file)" = "/run/secrets/nats.seed" ] \
    || fail "config.$service.toml.example does not name the seed its compose mount provides"
  toml_key_exists "$template" nats nkey \
    || fail "config.$service.toml.example has no [nats] nkey for provisioning to fill"
done
[ "$(toml_value "$ROOT/argus-deploy/config.tunnel.toml.example" nats url)" = "nats://127.0.0.1:4222" ] \
  || fail "the host-networked tunnel client does not reach the broker through its loopback publish"
! toml_key_exists "$ROOT/argus-deploy/config.tunnel.toml.example" nats password \
  || fail "the tunnel client carries the shared NATS password; it connects with its own key alone"

mkdir -p "$TEST_TMP/users/deploy" "$TEST_TMP/users/data/nats/seeds"
for service in $(nats_clients); do
  cp "$ROOT/argus-deploy/config.$service.toml.example" "$TEST_TMP/users/deploy/config.$service.toml"
done
cp "$ROOT/argus-deploy/config.tts.toml.example" "$TEST_TMP/users/deploy/config.tts.toml"
nats_auth_conf "$permissions" "$TEST_TMP/users/data/nats/seeds" "$legacy" >/dev/null \
  || fail "the NATS users could not be rendered for the key check"
{
  printf 'source %q\nsource %q\n' "$ROOT/scripts/lib/common.sh" "$ROOT/scripts/lib/nats-users.sh"
  printf 'DATA_DIR=%q\nDEPLOY_DIR=%q\n' "$TEST_TMP/users/data" "$TEST_TMP/users/deploy"
  awk '/^configure_nats_users\(\) \{$/ { on = 1 } on { print } on && /^}$/ { on = 0 }' "$ROOT/scripts/provision-host.sh"
  printf 'configure_nats_users\n'
} > "$TEST_TMP/users/run.sh"
bash "$TEST_TMP/users/run.sh" || fail "configure_nats_users failed"
for service in $(nats_clients); do
  [ "$(toml_value "$TEST_TMP/users/deploy/config.$service.toml" nats nkey)" \
      = "$(nkey_public_of "$TEST_TMP/users/data/nats/seeds/$service.seed")" ] \
    || fail "provisioning did not give config.$service.toml the public key of its own seed"
done
! toml_key_exists "$TEST_TMP/users/deploy/config.tts.toml" nats nkey \
  || fail "provisioning gave a NATS key to a service that does not connect to NATS"
grep -Eq '^  configure_nats_users$' "$ROOT/scripts/provision-host.sh" \
  || fail "provision-host.sh does not write each service's NATS key into its config"

image_of() {
  service_block "$COMPOSE" "$1" | sed -n 's/^    image: //p'
}
[ -n "$(image_of rustfs)" ] && [ "$(image_of rustfs-init)" = "$(image_of rustfs)" ] \
  || fail "rustfs-init does not run on the pinned RustFS image, whose curl it signs with"
! grep -Eq '(^|[^a-z_])rc[[:space:]]' "$ROOT/scripts/lib/s3-init.sh" \
  || fail "s3-init.sh calls rc, which takes keys only as arguments"
holds_key() {
  grep -Eq '^[[:space:]]*[A-Za-z_]+=\$\((secret |[^)]*\$secrets/)|read[^<]*<[^)]*\$secrets/' "$1"
}
! holds_key "$ROOT/scripts/lib/s3-init.sh" || fail "s3-init.sh holds a key in a shell variable"
printf 'leak=$(secret argus_s3_secret_key)\n' > "$TEST_TMP/held-key.sh"
holds_key "$TEST_TMP/held-key.sh" || fail "a key held in a shell variable was not reported"

grep -qx '\${ARGUS_DATA_DIR:-./data}/rustfs/accounts /run/argus/accounts 0' <<<"$(bind_mounts "$COMPOSE" rustfs-init)" \
  || fail "rustfs-init does not mount the writers' account directory writable"
grep -Eq '^      RUSTFS_ACCOUNTS_DIR: /run/argus/accounts$' <<<"$(service_block "$COMPOSE" rustfs-init)" \
  || fail "rustfs-init is not told where the writers' accounts go"
s3_writers="identity camera guard"
misplaced_accounts() {
  local compose="$1" name service expected
  for name in $(service_names "$compose"); do
    [ "$name" = rustfs-init ] && continue
    service="${name#argus-}"
    expected=""
    case " $s3_writers " in
      *" $service "*) expected="\${ARGUS_DATA_DIR:-./data}/rustfs/accounts/$service /run/secrets/s3 1" ;;
    esac
    [ "$(bind_mounts "$compose" "$name" | grep '/rustfs/accounts' || true)" = "$expected" ] || echo "$name"
  done
}
[ -z "$(misplaced_accounts "$COMPOSE")" ] \
  || fail "services whose S3 account mount is missing, writable or not their own: $(misplaced_accounts "$COMPOSE" | tr '\n' ' ')"
sed 's#rustfs/accounts/camera#rustfs/accounts/guard#' "$COMPOSE" > "$TEST_TMP/compose-borrowed-account.yml"
[ "$(misplaced_accounts "$TEST_TMP/compose-borrowed-account.yml")" = "argus-camera" ] \
  || fail "a writer mounting another writer's account was not reported"
grep -v 'rustfs/accounts/identity' "$COMPOSE" > "$TEST_TMP/compose-no-account.yml"
[ "$(misplaced_accounts "$TEST_TMP/compose-no-account.yml")" = "argus-identity" ] \
  || fail "a writer without its account mount was not reported"
awk '/^  argus-guard:$/ { guard = 1 } guard && /rustfs\/accounts\/guard$/ { seen = 1 } seen && /^        read_only: true$/ { seen = 0; guard = 0; next } { print }' \
  "$COMPOSE" > "$TEST_TMP/compose-writable-account.yml"
[ "$(misplaced_accounts "$TEST_TMP/compose-writable-account.yml")" = "argus-guard" ] \
  || fail "a writer mounting its account writable was not reported"
for service in $s3_writers; do
  [ "$(toml_value "$ROOT/argus-deploy/config.$service.toml.example" storage.s3 credentials_file)" \
      = "/run/secrets/s3/credentials.json" ] \
    || fail "config.$service.toml.example does not name the credentials file its account mount provides"
done
grep -qx '  file="$dir/credentials.json"' "$ROOT/scripts/lib/s3-init.sh" \
  || fail "rustfs-init no longer writes the credentials file the writers read"
while read -r source prefix; do
  grep -qF "\"$prefix" "$ROOT/$source" || fail "$source no longer writes under $prefix, which a writer's account is scoped to"
done <<'LITERALS'
packages/lib/storage/src/storage/s3-storage-service.cc portraits/
services/identity/src/shared/services/face-crop/face-crop-store.cc faces/
services/camera/src/feature/operator/services/evidence/evidence-uploader.cc cameras/
services/guard/src/feature/guard/guard-service.cc guard/incidents/
LITERALS
for account in "identity portraits/ faces/" "camera cameras/" "guard guard/incidents/"; do
  grep -qx "ensure_account $account" "$ROOT/scripts/lib/s3-init.sh" \
    || fail "rustfs-init does not scope the ${account%% *} account to ${account#* }"
done

internal_tls_services() {
  local template name
  for template in "$ROOT"/argus-deploy/config.*.toml.example; do
    grep -qx '\[internal_tls\]' "$template" || continue
    name="${template##*/config.}"
    printf '%s\n' "${name%.toml.example}"
  done
}

misplaced_internal_tls() {
  local compose="$1" listeners name service expected
  listeners="$(internal_tls_services)"
  for name in $(service_names "$compose"); do
    service="${name#argus-}"
    expected=""
    if grep -qx "$service" <<<"$listeners"; then
      expected="\${ARGUS_DATA_DIR:-./data}/certs/internal/$service /run/secrets/internal-tls 1"
    fi
    [ "$(bind_mounts "$compose" "$name" | grep '/certs/internal' || true)" = "$expected" ] || echo "$name"
  done
}

loopback_publishes() {
  service_block "$1" "$2" | awk '
    /^      - "127\.0\.0\.1:/ {
      entry = $0
      gsub(/"/, "", entry)
      sub(/\/(tcp|udp)$/, "", entry)
      sub(/^.*:/, "", entry)
      print entry
    }'
}

internal_tls_wiring() {
  local service="$1" template="$2" native_template="$3" compose="$4" plain tls published
  plain="$(toml_value "$template" server grpc_port)"
  [ -n "$plain" ] || plain="$(toml_value "$template" rpc address)"
  plain="${plain##*:}"
  if [ -z "$plain" ]; then
    echo "config.$service.toml.example declares [internal_tls] without a plaintext gRPC port"
    return
  fi
  tls=$(( plain + 200 ))
  published="$(loopback_publishes "$compose" "argus-$service")"
  [ "$(toml_value "$template" internal_tls dir)" = /run/secrets/internal-tls ] \
    || echo "config.$service.toml.example does not read its certificates where its mount puts them"
  [ "$(toml_value "$template" internal_tls address)" = "0.0.0.0:$tls" ] \
    || echo "config.$service.toml.example does not listen for mutual TLS on 0.0.0.0:$tls"
  [ -z "$(toml_value "$native_template" internal_tls dir)" ] \
    || echo "services/$service/config.toml.example names the certificate directory setup.sh writes"
  [ "$(toml_value "$native_template" internal_tls address)" = "127.0.0.1:$tls" ] \
    || echo "services/$service/config.toml.example does not listen for mutual TLS on 127.0.0.1:$tls"
  if grep -qx "$plain" <<<"$published"; then
    grep -qx "$tls" <<<"$published" \
      || echo "argus-$service publishes its plaintext gRPC port $plain on 127.0.0.1 but not $tls"
  else
    ! grep -qx "$tls" <<<"$published" \
      || echo "argus-$service publishes $tls although its plaintext gRPC port $plain is not published"
  fi
}

[ -n "$(internal_tls_services)" ] || fail "no deploy template declares [internal_tls]"
[ -z "$(misplaced_internal_tls "$COMPOSE")" ] \
  || fail "services whose internal TLS mount is missing, writable or not their own: $(misplaced_internal_tls "$COMPOSE" | tr '\n' ' ')"
for service in $(internal_tls_services); do
  problems="$(internal_tls_wiring "$service" "$ROOT/argus-deploy/config.$service.toml.example" \
    "$ROOT/services/$service/config.toml.example" "$COMPOSE")"
  [ -z "$problems" ] || fail "$(tr '\n' ';' <<<"$problems")"
done
sed 's#certs/internal/tts#certs/internal/stt#' "$COMPOSE" > "$TEST_TMP/compose-borrowed-tls.yml"
[ "$(misplaced_internal_tls "$TEST_TMP/compose-borrowed-tls.yml")" = "argus-tts" ] \
  || fail "a service mounting another service's certificate directory was not reported"
grep -v 'certs/internal/tts' "$COMPOSE" > "$TEST_TMP/compose-no-tls.yml"
[ "$(misplaced_internal_tls "$TEST_TMP/compose-no-tls.yml")" = "argus-tts" ] \
  || fail "a mutual TLS listener without its certificate mount was not reported"
awk '/^  argus-tts:$/ { tts = 1 } tts && /certs\/internal\/tts$/ { seen = 1 } seen && /^        read_only: true$/ { seen = 0; tts = 0; next } { print }' \
  "$COMPOSE" > "$TEST_TMP/compose-writable-tls.yml"
[ "$(misplaced_internal_tls "$TEST_TMP/compose-writable-tls.yml")" = "argus-tts" ] \
  || fail "a certificate directory mounted writable was not reported"
sed 's/^address = "0.0.0.0:7329"$/address = "0.0.0.0:7330"/' "$ROOT/argus-deploy/config.tts.toml.example" \
  > "$TEST_TMP/tts-off-port.toml"
[ "$(internal_tls_wiring tts "$TEST_TMP/tts-off-port.toml" "$ROOT/services/tts/config.toml.example" "$COMPOSE")" \
    = "config.tts.toml.example does not listen for mutual TLS on 0.0.0.0:7329" ] \
  || fail "a mutual TLS port that is not the plaintext gRPC port plus 200 was not reported"
awk '{ print } /^  argus-tts:$/ { tts = 1 } tts && /^    ports:$/ { print "      - \"127.0.0.1:${TTS_TLS_PORT:-7329}:7329\""; tts = 0 }' \
  "$COMPOSE" > "$TEST_TMP/compose-published-tls.yml"
[ "$(internal_tls_wiring tts "$ROOT/argus-deploy/config.tts.toml.example" "$ROOT/services/tts/config.toml.example" \
    "$TEST_TMP/compose-published-tls.yml")" \
    = "argus-tts publishes 7329 although its plaintext gRPC port 7129 is not published" ] \
  || fail "a mutual TLS port published where the plaintext gRPC port is not was not reported"

sandbox="$TEST_TMP/provision"
mkdir -p "$sandbox/root/scripts" "$sandbox/root/argus-deploy" "$sandbox/bin"
cp "$ROOT/scripts/provision-host.sh" "$sandbox/root/scripts/"
cp -R "$ROOT/scripts/lib" "$ROOT/scripts/privacy" "$sandbox/root/scripts/"
cp "$ROOT"/argus-deploy/*.example "$ROOT/argus-deploy/docker-compose.yml" \
  "$ROOT/argus-deploy/nats.conf" "$ROOT/argus-deploy/nats-permissions.conf" "$sandbox/root/argus-deploy/"
grep -v '^credentials_file = ' "$ROOT/argus-deploy/config.identity.toml.example" \
  > "$sandbox/root/argus-deploy/config.identity.toml"
chmod 600 "$sandbox/root/argus-deploy/config.identity.toml"
awk '/^\[internal_tls\]$/ { skip = 1; next } skip && /^\[/ { skip = 0 } !skip { print }' \
  "$ROOT/argus-deploy/config.tts.toml.example" > "$sandbox/root/argus-deploy/config.tts.toml"
chmod 600 "$sandbox/root/argus-deploy/config.tts.toml"
! grep -q 'internal_tls\|7329' "$sandbox/root/argus-deploy/config.tts.toml" \
  || fail "the sandbox could not plant a tts config from before [internal_tls]"
for service in identity camera guard; do
  mkdir -p "$sandbox/data/rustfs/accounts/$service"
  {
    printf '{"credentials":{"accessKey":"'
    openssl rand -hex 10 | tr 'a-f' 'A-F' | tr -d '\n'
    printf '","secretKey":"'
    openssl rand -hex 20 | tr -d '\n'
    printf '"}}\n'
  } > "$sandbox/data/rustfs/accounts/$service/credentials.json"
done
for stub in docker sudo; do
  printf '#!/bin/sh\nexit 1\n' > "$sandbox/bin/$stub"
  chmod 700 "$sandbox/bin/$stub"
done
provision_dry_run() {
  PATH="$sandbox/bin:$PATH" \
  ARGUS_DATA_DIR="$sandbox/data" \
  ARGUS_CERTS_DIR="$sandbox/certs" \
  ARGUS_MODELS_DIR="$sandbox/models" \
  ARGUS_GO2RTC_DIR="$sandbox/go2rtc" \
  ARGUS_MDNS_ADDRESS=192.168.1.20 \
    bash -x "$sandbox/root/scripts/provision-host.sh" --no-docker -y --accept-privacy-notice --with-tunnel \
    > "$sandbox/out-$1" 2> "$sandbox/trace-$1" \
    || fail "the provisioning dry run ($1) failed: $(tail -n 3 "$sandbox/out-$1" | tr '\n' ' ')"
  grep -q 'storage.s3\|nats' "$sandbox/trace-$1" || fail "the provisioning dry run ($1) left no trace"
}

provisioned_state() {
  {
    cat "$sandbox/root/argus-deploy"/config.*.toml "$sandbox/root/argus-deploy/livekit-keys.yaml" \
      "$sandbox/data/nats/auth.conf" "$sandbox/data/nats/seeds"/*.seed "$sandbox/data/pki/pairing.code" \
      "$sandbox/data/rustfs/secrets"/*
    find "$sandbox/data/certs/internal" -type f | sort | xargs cat
  } | sha256sum
}

secret_values() {
  local config
  for config in "$@"; do
    awk -v file="${config#"$TEST_TMP"/}" '
      /^[[:space:]]*\[/ {
        table = $0
        gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", table)
        next
      }
      /^[[:space:]]*[A-Za-z0-9_.-]+[[:space:]]*=[[:space:]]*"/ {
        key = $0
        sub(/^[[:space:]]*/, "", key)
        sub(/[[:space:]]*=.*$/, "", key)
        if (key !~ /(^|_)(secret|password|credential)$/ && key !~ /^caller_/ && table != "rpc.callers")
          next
        value = $0
        sub(/^[^=]*=[[:space:]]*"/, "", value)
        sub(/".*$/, "", value)
        if (length(value) >= 8 && value !~ /CHANGE_ME/)
          printf "%s [%s] %s\t%s\n", file, table, key, value
      }' "$config"
  done
}

provisioned_secrets() {
  local seed name
  secret_values "$sandbox/root/argus-deploy"/config.*.toml
  for seed in "$sandbox/data/nats/seeds"/*.seed; do
    printf 'NATS seed %s\t%s\n' "${seed##*/}" "$(tr -d '[:space:]' < "$seed")"
    printf 'NATS seed payload %s\t%s\n' "${seed##*/}" "$(nkey_seed_payload < "$seed")"
  done
  printf 'pairing code\t%s\n' "$(tr -d '[:space:]' < "$sandbox/data/pki/pairing.code")"
  for name in rustfs_access_key rustfs_secret_key rustfs_rpc_secret argus_s3_access_key argus_s3_secret_key; do
    printf 'S3 %s\t%s\n' "$name" "$(tr -d '\r\n' < "$sandbox/data/rustfs/secrets/$name")"
  done
}

secret_named() {
  awk -F '\t' -v name="$2" '$1 == name { found = 1 } END { exit !found }' "$1"
}

leaked_secrets() {
  local inventory="$1" trace="$2" name value
  while IFS="$(printf '\t')" read -r name value; do
    if grep -qF -- "$value" "$trace"; then
      printf '%s; ' "$name"
    fi
  done < "$inventory"
}

check_trace() {
  local inventory="$1" trace="$2" what="$3"
  cut -f2 "$inventory" | sort -u > "$inventory.values"
  if grep -qF -f "$inventory.values" "$trace"; then
    fail "the set -x trace of $what carries: $(leaked_secrets "$inventory" "$trace")"
  fi
}

provision_dry_run first
first_state="$(provisioned_state)"
provision_dry_run second
[ "$(provisioned_state)" = "$first_state" ] || fail "a second provisioning run changed a config or a secret"
for name in rustfs_access_key rustfs_secret_key rustfs_rpc_secret argus_s3_access_key argus_s3_secret_key; do
  secret_file="$sandbox/data/rustfs/secrets/$name"
  [ -s "$secret_file" ] || fail "the provisioning dry run wrote no $name"
  [ "$(find "$secret_file" -perm 600)" = "$secret_file" ] || fail "$name is not 0600"
done
provisioned_secrets > "$sandbox/secrets.tsv"
for name in "provision/root/argus-deploy/config.auth.toml [jwt] secret" \
    "provision/root/argus-deploy/config.auth.toml [jwt] refresh_secret" \
    "provision/root/argus-deploy/config.auth.toml [device] fingerprint_secret" \
    "provision/root/argus-deploy/config.auth.toml [nats] password" \
    "provision/root/argus-deploy/config.sync.toml [rtc] api_secret" \
    "provision/root/argus-deploy/config.tunnel.toml [tunnel] secret" \
    "provision/root/argus-deploy/config.identity.toml [rpc.callers] settings" \
    "pairing code" "S3 argus_s3_secret_key"; do
  secret_named "$sandbox/secrets.tsv" "$name" || fail "the provisioned secret inventory has no $name"
done
[ "$(grep -c '^NATS seed payload ' "$sandbox/secrets.tsv")" -ge 10 ] \
  || fail "the provisioned secret inventory has fewer than 10 NATS seeds"
[ "$(awk -F '\t' '$1 ~ /credential$|\[rpc\.callers\]|caller_/' "$sandbox/secrets.tsv" | wc -l)" -ge 40 ] \
  || fail "the provisioned secret inventory has fewer than 40 RPC credentials"
check_trace "$sandbox/secrets.tsv" "$sandbox/trace-first" "the first provisioning run"
check_trace "$sandbox/secrets.tsv" "$sandbox/trace-second" "a second provisioning run"

generated_secret_files() {
  find "$@" -type f \( -name '*.key' -o -name 'key.pem' -o -name '*.seed' -o -name 'pairing.code' \
    -o -path '*/rustfs/secrets/*' -o -path '*/rustfs/accounts/*' \) | sort
}

secret_file_values() {
  local file label
  while read -r file; do
    label="${file#"$TEST_TMP"/}"
    case "$file" in
      *.json)
        awk -v label="$label" '
          { text = text $0 }
          END {
            while (match(text, /"(accessKey|secretKey)"[[:space:]]*:[[:space:]]*"[^"]+"/)) {
              value = substr(text, RSTART, RLENGTH)
              text = substr(text, RSTART + RLENGTH)
              sub(/^"[A-Za-z]+"[[:space:]]*:[[:space:]]*"/, "", value)
              sub(/"$/, "", value)
              printf "%s\t%s\n", label, value
            }
          }' "$file"
        ;;
      *.seed)
        printf '%s\t%s\n' "$label" "$(tr -d '[:space:]' < "$file")"
        printf '%s payload\t%s\n' "$label" "$(nkey_seed_payload < "$file")"
        ;;
      *)
        awk -v label="$label" '
          /^-----/ { next }
          {
            sub(/\r$/, "")
            gsub(/^[[:space:]]+|[[:space:]]+$/, "")
          }
          length($0) >= 8 { printf "%s\t%s\n", label, $0 }' "$file"
        ;;
    esac
  done
}

with_secret_parts() {
  awk -F '\t' '
    {
      print
      count = split($2, part, /[^A-Za-z0-9]+/)
      for (i = 1; i <= count; i++)
        if (length(part[i]) >= 16 && part[i] != $2)
          printf "%s part\t%s\n", $1, part[i]
    }'
}

generated_secret_files "$sandbox/data" "$sandbox/certs" | secret_file_values | with_secret_parts \
  > "$sandbox/secret-files.tsv"
for pattern in '/rustfs/secrets/' '/rustfs/accounts/' '/nats/seeds/' '/pairing[.]code$' '[.]key$'; do
  awk -F '\t' -v pattern="$pattern" '$1 ~ pattern { found = 1 } END { exit !found }' "$sandbox/secret-files.tsv" \
    || fail "the secret file values gathered nothing for $pattern"
done
[ "$(grep -c '/rustfs/secrets/' "$sandbox/secret-files.tsv")" -ge 6 ] \
  || fail "the secret file values miss a RustFS secret"
[ "$(grep -c '/rustfs/accounts/' "$sandbox/secret-files.tsv")" -eq 6 ] \
  || fail "the secret file values miss a writer account key"
[ "$(grep -c '/nats/seeds/' "$sandbox/secret-files.tsv")" -ge 20 ] \
  || fail "the secret file values miss a NATS seed"
[ "$(awk -F '\t' '$1 ~ /\/certs\/internal\/[a-z]+\/key[.]pem$/' "$sandbox/secret-files.tsv" | cut -f1 | sort -u | wc -l)" -eq 13 ] \
  || fail "the secret file values miss an internal service key"
check_trace "$sandbox/secret-files.tsv" "$sandbox/trace-first" "the first provisioning run (secret files)"
check_trace "$sandbox/secret-files.tsv" "$sandbox/trace-second" "a second provisioning run (secret files)"
{
  cat "$sandbox/trace-first"
  printf '+ replace_toml_value nats password %s\n' "$(awk -F '\t' '$1 ~ /\[nats\] password$/ { print $2; exit }' "$sandbox/secrets.tsv")"
} > "$TEST_TMP/planted-trace"
! (check_trace "$sandbox/secrets.tsv" "$TEST_TMP/planted-trace" "a planted trace") 2>/dev/null \
  || fail "a secret planted in a trace was not reported"

native="$TEST_TMP/native"
mkdir -p "$native/scripts"
cp -R "$ROOT/scripts/lib" "$native/scripts/"
for service in auth identity sync camera guard settings productivity notification tts stt vlm llm voice tunnel; do
  mkdir -p "$native/services/$service"
  cp "$ROOT/services/$service/config.toml.example" "$native/services/$service/"
done
{
  printf 'set -euo pipefail\nsource %q\nsource %q\nROOT=%q\n' "$native/scripts/lib/common.sh" \
    "$native/scripts/lib/internal-pki.sh" "$native"
  awk '/^(ensure_local_config|sync_storage_credentials)\(\) \{$/ { on = 1 } on { print } on && /^}$/ { on = 0 }' \
    "$ROOT/scripts/setup.sh"
  printf 'ensure_local_config\nsync_storage_credentials\n'
  grep -E '^  internal_pki_configure_native ' "$ROOT/scripts/setup.sh" | sed 's/^  //' || true
} > "$native/run.sh"
grep -q '^internal_pki_configure_native ' "$native/run.sh" \
  || fail "setup.sh no longer points the native configs at their internal certificates"
grep -q '^sync_storage_credentials() {$' "$native/run.sh" \
  || fail "setup.sh no longer defines the native config steps this test runs"
native_dry_run() {
  ARGUS_DATA_DIR="$sandbox/data" bash -x "$native/run.sh" > "$native/out-$1" 2> "$native/trace-$1" \
    || fail "the native setup dry run ($1) failed: $(tail -n 3 "$native/out-$1" | tr '\n' ' ')"
}
mkdir -p "$native/argus-deploy"
for service in identity camera; do
  cp "$ROOT/argus-deploy/config.$service.toml.example" "$native/argus-deploy/config.$service.toml"
done
deploy_state="$(sha256sum "$native"/argus-deploy/config.*.toml)"
native_dry_run first
native_state="$(cat "$native"/services/*/config.toml | sha256sum)"
native_dry_run second
[ "$(cat "$native"/services/*/config.toml | sha256sum)" = "$native_state" ] \
  || fail "a second native setup changed a config"
[ "$(sha256sum "$native"/argus-deploy/config.*.toml)" = "$deploy_state" ] \
  || fail "the native setup changed a container config, which belongs to provision-host.sh"
for run in first second; do
  [ "$(grep -cF 'The container configs argus-deploy/config.*.toml belong to provision-host.sh; setup.sh leaves them untouched.' \
      "$native/out-$run")" = 1 ] \
    || fail "the native setup did not say once that it leaves the container configs alone ($run run)"
done
[ "$(toml_value "$native/services/identity/config.toml" storage.s3 secret_key)" \
    = "$(tr -d '\r\n' < "$sandbox/data/rustfs/secrets/argus_s3_secret_key")" ] \
  || fail "the native setup did not sync the S3 secret into identity's config"
for service in $(internal_tls_services); do
  [ "$(toml_value "$native/services/$service/config.toml" internal_tls dir)" \
      = "$(cd "$sandbox/data/certs/internal" && pwd -P)/$service" ] \
    || fail "the native setup did not point $service at its internal certificate directory"
done
{
  secret_values "$native"/services/*/config.toml
  grep '^S3 ' "$sandbox/secrets.tsv"
} > "$native/secrets.tsv"
for name in "native/services/auth/config.toml [jwt] secret" \
    "native/services/auth/config.toml [device] fingerprint_secret" \
    "native/services/sync/config.toml [rtc] api_secret" \
    "native/services/tunnel/config.toml [tunnel] secret"; do
  secret_named "$native/secrets.tsv" "$name" || fail "the native secret inventory has no $name"
done
check_trace "$native/secrets.tsv" "$native/trace-first" "the native setup"
check_trace "$native/secrets.tsv" "$native/trace-second" "a second native setup"
{
  generated_secret_files "$native/services" | secret_file_values | with_secret_parts
  grep '/rustfs/secrets/' "$sandbox/secret-files.tsv"
} > "$native/secret-files.tsv"
awk -F '\t' '$1 ~ /link\.key$/ { found = 1 } END { exit !found }' "$native/secret-files.tsv" \
  || fail "the native secret file values miss the link key"
check_trace "$native/secret-files.tsv" "$native/trace-first" "the native setup (secret files)"
check_trace "$native/secret-files.tsv" "$native/trace-second" "a second native setup (secret files)"
for service in identity camera guard; do
  account_dir="$sandbox/data/rustfs/accounts/$service"
  [ -d "$account_dir" ] && [ "$(find "$account_dir" -maxdepth 0 -perm 700)" = "$account_dir" ] \
    || fail "provisioning did not create the 0700 account directory of $service"
  for pair in "access_key argus_s3_access_key" "secret_key argus_s3_secret_key" "bucket rustfs-bucket"; do
    set -- $pair
    [ "$(toml_value "$sandbox/root/argus-deploy/config.$service.toml" storage.s3 "$1")" \
        = "$(tr -d '\r\n' < "$sandbox/data/rustfs/secrets/$2")" ] \
      || fail "provisioning did not write $1 into config.$service.toml"
  done
  [ "$(toml_value "$sandbox/root/argus-deploy/config.$service.toml" storage.s3 credentials_file)" \
      = "/run/secrets/s3/credentials.json" ] \
    || fail "provisioning did not name the account's credentials file in config.$service.toml"
done
grep -Eq '^ensure_secret_file "\$secrets/argus_s3_secret_key" lower_hex 16$|^  ensure_secret_file "\$secrets/argus_s3_secret_key" lower_hex 16$' \
  "$ROOT/scripts/provision-host.sh" || fail "provision-host.sh no longer generates the application secret into its file"
source "$ROOT/scripts/lib/internal-pki.sh"
for service in $INTERNAL_PKI_SERVICES; do
  openssl verify -CAfile "$sandbox/data/certs/internal/trust.pem" "$sandbox/data/certs/internal/$service/cert.pem" \
    > /dev/null 2>&1 || fail "provisioning did not issue the internal certificate of $service"
done
for service in $(internal_tls_services); do
  grep -qw "$service" <<<"$INTERNAL_PKI_SERVICES" \
    || fail "$service listens for mutual TLS but the internal PKI issues it no certificate"
  for key in dir address; do
    [ "$(toml_value "$sandbox/root/argus-deploy/config.$service.toml" internal_tls "$key")" \
        = "$(toml_value "$ROOT/argus-deploy/config.$service.toml.example" internal_tls "$key")" ] \
      || fail "provisioning did not bring [internal_tls] $key into config.$service.toml"
  done
done

source "$ROOT/scripts/lib/nats-server-binary.sh"
nats_check_skip=""
if nats_server_binary_find; then
  mkdir -p "$TEST_TMP/broker/js"
  cp "$permissions" "$TEST_TMP/broker/"
  nats_auth_conf "$permissions" "$TEST_TMP/broker" "$legacy" > "$TEST_TMP/broker/auth.conf"
  sed -e "s#\"/data\"#\"$TEST_TMP/broker/js\"#" "$ROOT/argus-deploy/nats.conf" > "$TEST_TMP/broker/nats.conf"
  "$NATS_SERVER_BIN" -c "$TEST_TMP/broker/nats.conf" -t > "$TEST_TMP/broker/check.out" 2>&1 \
    || fail "nats-server refuses the provisioned configuration: $(tail -n 1 "$TEST_TMP/broker/check.out")"
  printf 'deploy-mounts-test: nats-server accepts the provisioned configuration\n'
else
  nats_server_binary_fail_in_ci deploy-mounts-test
  nats_check_skip="SKIPPED: deploy-mounts-test: the nats-server check of the provisioned configuration, $NATS_SERVER_MISSING"
fi

printf 'deploy-mounts-test: ok\n'
if [ -n "$nats_check_skip" ]; then
  printf '%s\n' "$nats_check_skip"
  exit 77
fi
