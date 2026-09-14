#!/usr/bin/env bash
#
# Argus host provisioning for the argus-deploy stack.
#
# Prepares a Linux or macOS host with the state the deployment needs but never
# bakes into images: Docker + Compose v2, the instance PKI, the per-service
# config files with unique shared secrets and the external data tree the
# compose file links into the containers. Idempotent: an existing CA, secret or
# database is reused untouched, so an image update is pull/build plus `up -d`.
#
# Usage:
#   ./scripts/provision-host.sh                     # prepare everything, no start
#   ./scripts/provision-host.sh --start             # build + start the stack
#   ./scripts/provision-host.sh --data-dir /srv/argus
#   ./scripts/provision-host.sh --with-models       # download engine weights
#   ./scripts/provision-host.sh --migrate-volumes   # copy old named volumes
#   ./scripts/provision-host.sh --no-docker -y
#   ./scripts/provision-host.sh --no-s3             # skip the object store
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/pki.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/docker.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_DIR="$ROOT/argus-deploy"

ASSUME_YES="${ARGUS_ASSUME_YES:+1}"
WITH_DOCKER=1
WITH_MODELS=0
WITH_S3=1
START=0
MIGRATE_VOLUMES=0

usage() {
  sed -n '2,18p' "$0" | sed 's/^#\{1,2\} \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -y|--yes) ASSUME_YES=1; shift ;;
    --no-docker) WITH_DOCKER=0; shift ;;
    --with-models) WITH_MODELS=1; shift ;;
    --no-s3) WITH_S3=0; shift ;;
    --start) START=1; shift ;;
    --migrate-volumes) MIGRATE_VOLUMES=1; shift ;;
    --data-dir) [ "$#" -ge 2 ] || { err "--data-dir needs a path"; exit 2; }; DATA_DIR_IN="$2"; shift 2 ;;
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

ensure_data_tree() {
  local sub
  mkdir -p "$DATA_DIR"
  for sub in identity camera productivity notification guard memory; do
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
    awk -v key="$key" -v value="$value" '
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
  log "Compose environment ready: $env_file"
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

  configure_storage "$DEPLOY_DIR/config.gateway.toml" "http://127.0.0.1:9000"
  configure_storage "$DEPLOY_DIR/config.camera.toml" "http://rustfs:9000"
  configure_storage "$DEPLOY_DIR/config.guard.toml" "http://rustfs:9000"

  log "Object store ready: $root/objects"
}

provision_models() {  local owner script
  for owner in \
      packages/argus-identity \
      packages/argus-memory \
      services/argus-tts \
      services/argus-llm \
      services/argus-vlm \
      services/argus-stt \
      services/argus-voice \
      services/argus-camera; do
    script="$ROOT/$owner/scripts/provision.sh"
    if [ -f "$script" ]; then
      log "Provisioning models: $owner"
      bash "$script" || warn "Model provisioning failed for $owner; continuing."
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
  [ -f "$CERTS_DIR/pairing.code" ] && pairing="$(cat "$CERTS_DIR/pairing.code")"

  echo
  log "Host ready."
  printf '  data dir       : %s\n' "$DATA_DIR"
  printf '  certs dir      : %s\n' "$CERTS_DIR"
  printf '  models dir     : %s\n' "$MODELS_DIR"
  printf '  go2rtc dir     : %s\n' "$GO2RTC_DIR"
  printf '  objects dir    : %s\n' "$DATA_DIR/rustfs/objects"
  printf '  pairing code   : %s\n' "$pairing"
  echo
  log "Start : (cd argus-deploy && docker compose up -d)"
  log "Update: docker compose build && docker compose up -d"
  log "Logs  : docker compose logs -f gateway"
  log "Never run 'docker compose down -v': it deletes the camera stream volume."
}

main() {
  need_cmd openssl
  ensure_data_tree
  write_env
  ensure_instance_certs "$ROOT" "$CERTS_DIR" "$DEPLOY_DIR/config.gateway.toml"
  ensure_deploy_configs "$DEPLOY_DIR"
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
