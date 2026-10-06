#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/pki.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/docker.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/privacy.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_DIR="$ROOT/argus-deploy"

ASSUME_YES="${ARGUS_ASSUME_YES:+1}"
WITH_DOCKER=1
WITH_MODELS=0
WITH_S3=1
START=0
WITH_TUNNEL=0
MIGRATE_VOLUMES=0
PRIVACY_ACTION="require"

usage() {
  cat <<'USAGE'
Argus host provisioning for the argus-deploy stack.

Prepares a Linux or macOS host with the state the deployment needs but never
bakes into images: Docker + Compose v2, the instance PKI, the per-service
config files with unique shared secrets and the external data tree the
compose file links into the containers. Idempotent: an existing CA, secret or
database is reused untouched, so an image update is pull/build plus `up -d`.
The Argus privacy notice and terms are shown first; nothing is configured
until the owner accepts them for the household.

Usage:
  ./scripts/provision-host.sh                     prepare everything, no start
  ./scripts/provision-host.sh --start             build + start the stack
  ./scripts/provision-host.sh --data-dir /srv/argus
  ./scripts/provision-host.sh --mdns-address 192.168.1.20
  ./scripts/provision-host.sh --with-models       download engine weights
  ./scripts/provision-host.sh --with-tunnel       turn the remote tunnel profile on
  ./scripts/provision-host.sh --migrate-volumes   copy old named volumes
  ./scripts/provision-host.sh --no-docker -y
  ./scripts/provision-host.sh --no-s3             skip the object store
  ./scripts/provision-host.sh --accept-privacy-notice --no-docker -y

Privacy notice (required before anything is configured):
  --accept-privacy-notice      accept the notice in a non-interactive run
                               (also ARGUS_ACCEPT_PRIVACY_NOTICE=1); without it
                               a non-interactive run stops; -y never accepts it
  --accept-visitor-notice      also acknowledge recurring-visitor recognition
                               (it stays off until enabled in the app)
  --privacy-lang es|en         notice language (default: from LANG, else es)
  --jurisdiction pe            country rules quoted in the notice
                               (scripts/privacy/jurisdictions.tsv)
  --show-privacy-notice        print the notice and exit
  --withdraw-privacy-consent   remove the recorded acceptance and exit
  The acceptance (version, time, user@host) is stored in
  <data dir>/privacy/host-consent.json (0600).
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -y|--yes) ASSUME_YES=1; shift ;;
    --no-docker) WITH_DOCKER=0; shift ;;
    --with-models) WITH_MODELS=1; shift ;;
    --no-s3) WITH_S3=0; shift ;;
    --start) START=1; shift ;;
    --with-tunnel) WITH_TUNNEL=1; shift ;;
    --migrate-volumes) MIGRATE_VOLUMES=1; shift ;;
    --accept-privacy-notice) PRIVACY_ACCEPT=1; shift ;;
    --accept-visitor-notice) PRIVACY_VISITOR_ACCEPT=1; shift ;;
    --show-privacy-notice) PRIVACY_ACTION="show"; shift ;;
    --withdraw-privacy-consent) PRIVACY_ACTION="withdraw"; shift ;;
    --privacy-lang=*) PRIVACY_LANG="${1#*=}"; shift ;;
    --privacy-lang) [ "$#" -ge 2 ] || { err "--privacy-lang needs es or en"; exit 2; }; PRIVACY_LANG="$2"; shift 2 ;;
    --jurisdiction=*) PRIVACY_JURISDICTION="${1#*=}"; shift ;;
    --jurisdiction) [ "$#" -ge 2 ] || { err "--jurisdiction needs a code"; exit 2; }; PRIVACY_JURISDICTION="$2"; shift 2 ;;
    --data-dir) [ "$#" -ge 2 ] || { err "--data-dir needs a path"; exit 2; }; DATA_DIR_IN="$2"; shift 2 ;;
    --mdns-address) [ "$#" -ge 2 ] || { err "--mdns-address needs an IP"; exit 2; }; MDNS_ADDRESS_IN="$2"; shift 2 ;;
    *) err "unknown argument: $1"; usage >&2; exit 2 ;;
  esac
done

abs_path() {
  case "$1" in
    /*) printf '%s' "$1" ;;
    *) printf '%s/%s' "$ROOT" "$1" ;;
  esac
}

DATA_DIR="$(abs_path "${DATA_DIR_IN:-${ARGUS_DATA_DIR:-argus-deploy/data}}")"
CERTS_DIR="$(abs_path "${ARGUS_CERTS_DIR:-certs}")"
MODELS_DIR="$(abs_path "${ARGUS_MODELS_DIR:-models}")"
GO2RTC_DIR="$(abs_path "${ARGUS_GO2RTC_DIR:-third_party/go2rtc}")"
CA_DIR="$(abs_path "${ARGUS_CA_DIR:-$DATA_DIR/pki}")"
AUTH_TUNNEL_PORT="${AUTH_TUNNEL_PORT:-7142}"
IDENTITY_TUNNEL_PORT="${IDENTITY_TUNNEL_PORT:-7144}"

ensure_data_tree() {
  local sub
  mkdir -p "$DATA_DIR"
  for sub in identity auth camera productivity notification guard memory sync settings nats; do
    mkdir -p "$DATA_DIR/$sub"
  done
  chmod 700 "$DATA_DIR" "$DATA_DIR"/* 2>/dev/null || true
  migrate_identity_dir
  log "External data tree ready: $DATA_DIR"
}

stack_running() {
  command -v docker >/dev/null 2>&1 || return 1
  local docker_cmd
  docker_cmd="$(docker_client)"
  $docker_cmd ps --format '{{.Names}}' 2>/dev/null | grep -q '^argus-cutover-'
}

legacy_volumes_present() {
  command -v docker >/dev/null 2>&1 || return 1
  local docker_cmd
  docker_cmd="$(docker_client)"
  $docker_cmd volume ls --format '{{.Name}}' 2>/dev/null | grep -q '^argus-cutover-.*-db$'
}

migrate_identity_dir() {
  local target="$DATA_DIR/identity"
  local file

  if [ -n "$(ls -A "$target" 2>/dev/null)" ]; then
    return 0
  fi
  for file in identity.db identity.db-wal identity.db-shm; do
    [ -f "$DATA_DIR/$file" ] || continue
    if stack_running; then
      warn "argus-cutover stack is running; stop it, then re-run to move $file."
      return 0
    fi
    mv "$DATA_DIR/$file" "$target/$file"
    log "Moved legacy $DATA_DIR/$file -> $target/$file"
  done
}

ensure_env_value() {
  local file="$1"
  local key="$2"
  local value="$3"
  local temp

  if grep -q "^${key}=" "$file" 2>/dev/null; then
    temp="$(mktemp "${file}.tmp.XXXXXX")"
    ARGUS_ENV_VALUE="$value" awk -v key="$key" '
      BEGIN { value = ENVIRON["ARGUS_ENV_VALUE"] }
      $0 ~ "^" key "=" { print key "=" value; next }
      { print }' "$file" > "$temp"
    chmod 600 "$temp"
    mv "$temp" "$file"
    return
  fi
  printf '%s=%s\n' "$key" "$value" >> "$file"
}

write_env() {
  local env_file="$DEPLOY_DIR/.env"

  touch "$env_file"
  chmod 600 "$env_file"
  ensure_env_value "$env_file" ARGUS_UID "$(id -u)"
  ensure_env_value "$env_file" ARGUS_GID "$(id -g)"
  ensure_env_value "$env_file" ARGUS_DATA_DIR "$DATA_DIR"
  ensure_env_value "$env_file" ARGUS_CERTS_DIR "$CERTS_DIR"
  ensure_env_value "$env_file" ARGUS_MODELS_DIR "$MODELS_DIR"
  ensure_env_value "$env_file" ARGUS_GO2RTC_DIR "$GO2RTC_DIR"
  ensure_env_value "$env_file" ARGUS_CA_DIR "$CA_DIR"
  if grep -Eq '^COMPOSE_PROFILES=.*(^|[=,])tunnel(,|$)' "$env_file" 2>/dev/null; then
    WITH_TUNNEL=1
  fi
  if [ "$WITH_TUNNEL" -eq 1 ]; then
    ensure_compose_profile "$env_file" tunnel
  fi
  log "Compose environment ready: $env_file"
}

ensure_compose_profile() {
  local env_file="$1"
  local profile="$2"
  local current

  current="$(sed -n 's/^COMPOSE_PROFILES=//p' "$env_file" | head -1)"
  case ",$current," in
    *",$profile,"*) return 0 ;;
  esac
  ensure_env_value "$env_file" COMPOSE_PROFILES "${current:+$current,}$profile"
}

configure_identity_signer() {
  local config="$DEPLOY_DIR/config.identity.toml"

  [ -f "$config" ] || return 0
  case "$(toml_value "$config" cert ca_key)" in
    ""|certs/ca.key) replace_toml_value cert ca_key "ca/ca.key" "$config" ;;
  esac
  case "$(toml_value "$config" cert pairing_code)" in
    ""|certs/pairing.code) replace_toml_value cert pairing_code "ca/pairing.code" "$config" ;;
  esac
}

configure_tunnel() {
  local auth="$DEPLOY_DIR/config.auth.toml"
  local identity="$DEPLOY_DIR/config.identity.toml"
  local client="$DEPLOY_DIR/config.tunnel.toml"
  local relay="$DEPLOY_DIR/config.relay.toml"
  local config port

  for config in "$auth:$AUTH_TUNNEL_PORT" "$identity:$IDENTITY_TUNNEL_PORT"; do
    port="${config##*:}"
    config="${config%:*}"
    [ -f "$config" ] || continue
    case "$(toml_value "$config" remote tunnel_port)" in
      ""|0) replace_toml_value remote tunnel_port "$port" "$config" literal ;;
    esac
    replace_toml_value remote tunnel_profile true "$config" literal
  done
  if [ -f "$client" ]; then
    replace_toml_value server gateway_host "127.0.0.1" "$client"
    replace_toml_value server gateway_port "$AUTH_TUNNEL_PORT" "$client" literal
    remove_toml_key tunnel max_reconnects "$client"
  fi
  remove_toml_key tunnel max_reconnects "$relay"
  ensure_tunnel_secret "$client" "$relay"
  log "Tunnel profile on: argus-auth's remote listener on 127.0.0.1:$AUTH_TUNNEL_PORT, identity's on 127.0.0.1:$IDENTITY_TUNNEL_PORT"
}

write_nats_auth() {
  local dir="$DATA_DIR/nats"
  local password temp

  password="$(toml_value "$DEPLOY_DIR/config.auth.toml" nats password)"
  case "$password" in
    ""|*CHANGE_ME*) err "no NATS password in config.auth.toml"; return 1 ;;
  esac
  case "$password" in
    *[!A-Za-z0-9]*) err "the NATS password must be alphanumeric"; return 1 ;;
  esac
  mkdir -p "$dir"
  chmod 700 "$dir"
  temp="$(mktemp "$dir/auth.conf.XXXXXX")"
  printf 'authorization {\n  user: argus\n  password: "%s"\n}\n' "$password" > "$temp"
  chmod 644 "$temp"
  mv "$temp" "$dir/auth.conf"
}

detect_lan_address() {
  local address="${MDNS_ADDRESS_IN:-${ARGUS_MDNS_ADDRESS:-}}"

  if [ -z "$address" ] && command -v ip >/dev/null 2>&1; then
    address="$(ip route get 1.1.1.1 2>/dev/null \
      | sed -n 's/.* src \([0-9a-fA-F:.]*\).*/\1/p' | head -1)"
  fi
  if [ -z "$address" ] && command -v hostname >/dev/null 2>&1; then
    address="$(hostname -I 2>/dev/null | awk '{print $1}')"
  fi
  printf '%s' "$address"
}

configure_mdns_address() {
  local address="$1" config

  for config in "$DEPLOY_DIR"/config.*.toml; do
    [ -f "$config" ] || continue
    toml_key_exists "$config" mdns address || continue
    replace_toml_value mdns address "$address" "$config"
  done
}

configure_webrtc_candidate() {
  local address="$1" config="$DEPLOY_DIR/config.camera.toml"

  [ -f "$config" ] || return 0
  toml_key_exists "$config" streaming webrtc_candidates || return 0
  replace_toml_value streaming webrtc_candidates "$address:${CAMERA_WEBRTC_PORT:-8555}" "$config"
}

ensure_secret_file() {
  local path="$1"
  local value="$2"

  if [ -s "$path" ]; then
    chmod 600 "$path"
    return
  fi
  printf '%s\n' "$value" > "$path"
  chmod 600 "$path"
}

configure_storage() {
  local config="$1"
  local endpoint="$2"
  local secrets="$DATA_DIR/rustfs/secrets"
  local bucket access secret

  [ -f "$config" ] || return 0
  bucket="$(tr -d '\r\n' < "$secrets/rustfs-bucket")"
  access="$(tr -d '\r\n' < "$secrets/argus_s3_access_key")"
  secret="$(tr -d '\r\n' < "$secrets/argus_s3_secret_key")"

  if ! toml_key_exists "$config" storage mode; then
    printf '\n[storage]\nmode = "s3"\n\n[storage.s3]\nendpoint = "%s"\nregion = "us-east-1"\nbucket = "%s"\naccess_key = "%s"\nsecret_key = "%s"\n' \
      "$endpoint" "$bucket" "$access" "$secret" >> "$config"
    chmod 600 "$config"
    return
  fi

  replace_toml_value storage mode "s3" "$config"
  replace_toml_value storage.s3 endpoint "$endpoint" "$config"
  replace_toml_value storage.s3 region "us-east-1" "$config"
  replace_toml_value storage.s3 bucket "$bucket" "$config"
  replace_toml_value storage.s3 access_key "$access" "$config"
  replace_toml_value storage.s3 secret_key "$secret" "$config"
}

ensure_object_store() {
  local root="$DATA_DIR/rustfs"
  local secrets="$root/secrets"

  mkdir -p "$root/objects" "$secrets"
  chmod 700 "$root" "$secrets"

  ensure_secret_file "$secrets/rustfs_access_key" \
    "$(openssl rand -hex 20 | tr '[:lower:]' '[:upper:]')"
  ensure_secret_file "$secrets/rustfs_secret_key" "$(openssl rand -hex 48)"
  ensure_secret_file "$secrets/rustfs_rpc_secret" "$(openssl rand -hex 48)"
  ensure_secret_file "$secrets/argus_s3_access_key" \
    "$(openssl rand -hex 8 | tr '[:lower:]' '[:upper:]')"
  ensure_secret_file "$secrets/argus_s3_secret_key" "$(openssl rand -hex 16)"
  ensure_secret_file "$secrets/rustfs-bucket" \
    "argus-$(openssl rand -hex 10)-private"

  configure_storage "$DEPLOY_DIR/config.identity.toml" "http://rustfs:9000"
  configure_storage "$DEPLOY_DIR/config.camera.toml" "http://rustfs:9000"
  configure_storage "$DEPLOY_DIR/config.guard.toml" "http://rustfs:9000"

  log "Object store ready: $root/objects"
}

provision_models() {
  local owner script
  for owner in \
      services/identity \
      services/tts \
      services/llm \
      services/vlm \
      services/stt \
      services/voice \
      services/camera; do
    script="$ROOT/$owner/scripts/provision.sh"
    if [ -f "$script" ]; then
      log "Provisioning models: $owner"
      ARGUS_TTS_CONFIG="$DEPLOY_DIR/config.tts.toml" bash "$script" || \
        warn "Model provisioning failed for $owner; continuing."
    fi
  done
}

run_compose() {
  local docker_cmd
  docker_cmd="$(docker_client)"
  ( cd "$DEPLOY_DIR" && $docker_cmd compose "$@" )
}

migrate_named_volumes() {
  local docker_cmd pair volume subdir
  docker_cmd="$(docker_client)"

  for pair in \
      "argus-cutover-camera-db:camera" \
      "argus-cutover-productivity-db:productivity" \
      "argus-cutover-notification-db:notification" \
      "argus-cutover-guard-db:guard" \
      "argus-cutover-memory-db:memory"; do
    volume="${pair%%:*}"
    subdir="$DATA_DIR/${pair##*:}"
    $docker_cmd volume inspect "$volume" >/dev/null 2>&1 || continue
    [ -n "$(ls -A "$subdir" 2>/dev/null)" ] && continue
    log "Migrating named volume $volume -> $subdir"
    mkdir -p "$subdir"
    $docker_cmd run --rm -v "$volume:/from:ro" -v "$subdir:/to" alpine:3.20 \
      sh -c 'cp -a /from/. /to/ 2>/dev/null || true' \
      || warn "Failed to copy $volume; the volume is kept untouched."
  done
}

start_stack() {
  if ! docker_daemon_ok; then
    err "Docker is not reachable; cannot start the stack."
    return 1
  fi
  log "Building the stack images (sequential: shared Conan cache)..."
  COMPOSE_PARALLEL_LIMIT=1 run_compose build
  log "Starting the stack..."
  run_compose up -d
  run_compose ps
}

print_summary() {
  local pairing="unavailable"
  [ -f "$CA_DIR/pairing.code" ] && pairing="stored in $CA_DIR/pairing.code (0600)"

  echo
  log "Host ready."
  printf '  data dir       : %s\n' "$DATA_DIR"
  printf '  certs dir      : %s\n' "$CERTS_DIR"
  printf '  CA signer dir  : %s\n' "$CA_DIR"
  printf '  models dir     : %s\n' "$MODELS_DIR"
  printf '  go2rtc dir     : %s\n' "$GO2RTC_DIR"
  printf '  objects dir    : %s\n' "$DATA_DIR/rustfs/objects"
  printf '  pairing code   : %s\n' "$pairing"
  echo
  log "Start : (cd argus-deploy && docker compose up -d)"
  log "Update: docker compose build && docker compose up -d"
  log "Logs  : docker compose logs -f"
  log "Never run 'docker compose down -v': it deletes the camera stream volume."
}

main() {
  need_cmd openssl
  case "$PRIVACY_ACTION" in
    show) privacy_notice_text; exit $? ;;
    withdraw) privacy_withdraw_consent "$DATA_DIR"; exit 0 ;;
  esac
  privacy_require_consent "$DATA_DIR" "scripts/provision-host.sh"
  ensure_data_tree
  write_env
  ensure_deploy_configs "$DEPLOY_DIR"
  write_nats_auth
  configure_identity_signer
  if [ "$WITH_TUNNEL" -eq 1 ]; then
    configure_tunnel
  fi

  local lan_address
  lan_address="$(detect_lan_address)"
  if [ -n "$lan_address" ]; then
    configure_mdns_address "$lan_address"
    configure_webrtc_candidate "$lan_address"
    configure_lan_networks "$(host_lan_networks "$lan_address")" "$DEPLOY_DIR"/config.*.toml
    log "mDNS announcements and the camera WebRTC candidate carry $lan_address (override with ARGUS_MDNS_ADDRESS)"
  else
    warn "No LAN address detected; set ARGUS_MDNS_ADDRESS and re-run, or the"
    warn "containers announce their own bridge interface and stay undiscoverable"
  fi
  ensure_instance_certs "$ROOT" "$CERTS_DIR" "$DEPLOY_DIR/config.identity.toml" \
    "$CA_DIR" "$lan_address"
  if [ "$WITH_S3" -eq 1 ]; then
    ensure_object_store
  fi

  if [ "$WITH_MODELS" -eq 1 ]; then
    provision_models
  elif [ ! -d "$MODELS_DIR/llm" ] || [ ! -d "$MODELS_DIR/stt" ]; then
    warn "Model weights missing under $MODELS_DIR; run with --with-models to download."
  fi

  if [ "$WITH_DOCKER" -eq 1 ]; then
    ensure_docker "$ASSUME_YES" || warn "Docker not ready; provisioning continues."
  fi

  if [ "$MIGRATE_VOLUMES" -eq 1 ] || legacy_volumes_present; then
    if stack_running; then
      warn "Stack is running; stop it before copying the legacy named volumes."
    else
      migrate_named_volumes
    fi
  fi

  if [ "$START" -eq 1 ]; then
    start_stack
  fi

  print_summary
}

main "$@"
