#!/usr/bin/env bash
# Creates local-only RustFS instance state. It is safe to run repeatedly and
# never prints generated secret values.
set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
readonly RUNTIME_DIR="$ROOT_DIR/docker/runtime"
readonly SECRET_DIR="$RUNTIME_DIR/secrets"
readonly BUCKET_FILE="$RUNTIME_DIR/rustfs-bucket"
readonly LOCAL_CONFIG="$ROOT_DIR/config.local.toml"

log() { printf '[argus-storage] %s\n' "$*"; }
fail() { printf '[argus-storage] error: %s\n' "$*" >&2; exit 1; }

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

ensure_secure_file() {
  local path="$1"
  local value="$2"

  if [[ -L "$path" ]]; then
    fail "refusing symbolic link: $path"
  fi
  if [[ -e "$path" ]]; then
    [[ -s "$path" ]] || fail "existing file is empty: $path"
    chmod 600 "$path"
    return
  fi

  printf '%s\n' "$value" > "$path"
  chmod 600 "$path"
}

generate_access_key() {
  openssl rand -hex 20 | tr '[:lower:]' '[:upper:]'
}

generate_secret() {
  openssl rand -hex 48
}

generate_bucket() {
  printf 'argus-%s-private' "$(openssl rand -hex 10)"
}

write_local_config() {
  local bucket app_access_key app_secret_key
  bucket=$(tr -d '\r\n' < "$BUCKET_FILE")
  [[ -n "$bucket" ]] || fail "bucket file is empty: $BUCKET_FILE"
  app_access_key=$(tr -d '\r\n' < "$SECRET_DIR/argus_s3_access_key")
  app_secret_key=$(tr -d '\r\n' < "$SECRET_DIR/argus_s3_secret_key")
  [[ -n "$app_access_key" ]] || fail "application access key is empty"
  [[ -n "$app_secret_key" ]] || fail "application secret key is empty"

  if [[ ! -e "$LOCAL_CONFIG" ]]; then
    cat > "$LOCAL_CONFIG" <<EOF
[storage]
mode = "managed_local"

[storage.s3]
endpoint = "http://127.0.0.1:9000"
region = "us-east-1"
bucket = "$bucket"
access_key = "$app_access_key"
secret_key = "$app_secret_key"
EOF
    chmod 600 "$LOCAL_CONFIG"
    return
  fi

  [[ -s "$LOCAL_CONFIG" ]] || fail "existing config overlay is empty: $LOCAL_CONFIG"

  local temp_config
  temp_config=$(mktemp "${LOCAL_CONFIG}.tmp.XXXXXX")
  awk -v bucket="$bucket" -v app_access_key="$app_access_key" \
    -v app_secret_key="$app_secret_key" '
    function emit_missing() {
      if (!has_bucket) print "bucket = \"" bucket "\""
      if (!has_access_key) print "access_key = \"" app_access_key "\""
      if (!has_secret_key) print "secret_key = \"" app_secret_key "\""
    }
    /^\[storage\.s3\][[:space:]]*$/ {
      if (in_storage) emit_missing()
      in_storage = 1
      has_storage = 1
      has_bucket = has_access_key = has_secret_key = 0
      print
      next
    }
    in_storage && /^\[/ {
      emit_missing()
      in_storage = 0
    }
    in_storage && /^[[:space:]]*bucket[[:space:]]*=/ {
      print "bucket = \"" bucket "\""
      has_bucket = 1
      next
    }
    in_storage && /^[[:space:]]*access_key[[:space:]]*=/ {
      print "access_key = \"" app_access_key "\""
      has_access_key = 1
      next
    }
    in_storage && /^[[:space:]]*secret_key[[:space:]]*=/ {
      print "secret_key = \"" app_secret_key "\""
      has_secret_key = 1
      next
    }
    { print }
    END {
      if (in_storage) emit_missing()
      if (!has_storage) {
        print ""
        print "[storage.s3]"
        print "endpoint = \"http://127.0.0.1:9000\""
        print "region = \"us-east-1\""
        print "bucket = \"" bucket "\""
        print "access_key = \"" app_access_key "\""
        print "secret_key = \"" app_secret_key "\""
      }
    }' "$LOCAL_CONFIG" > "$temp_config"
  chmod 600 "$temp_config"
  mv "$temp_config" "$LOCAL_CONFIG"
}

main() {
  require_command docker
  docker compose version >/dev/null 2>&1 || fail "Docker Compose v2 is required"
  require_command openssl

  umask 077
  mkdir -p "$SECRET_DIR"
  chmod 700 "$RUNTIME_DIR" "$SECRET_DIR"

  ensure_secure_file "$SECRET_DIR/rustfs_access_key" "$(generate_access_key)"
  ensure_secure_file "$SECRET_DIR/rustfs_secret_key" "$(generate_secret)"
  ensure_secure_file "$SECRET_DIR/rustfs_rpc_secret" "$(generate_secret)"
  ensure_secure_file "$SECRET_DIR/argus_s3_access_key" "$(generate_access_key)"
  ensure_secure_file "$SECRET_DIR/argus_s3_secret_key" "$(generate_secret)"
  ensure_secure_file "$BUCKET_FILE" "$(generate_bucket)"
  write_local_config

  log "RustFS runtime directory ready: $RUNTIME_DIR"
  log "Secrets directory ready: $SECRET_DIR"
  log "Bucket descriptor ready: $BUCKET_FILE"
  log "Application storage credentials and overlay ready: $LOCAL_CONFIG"
  log "Next step: docker compose up -d rustfs rustfs-init"
}

main "$@"
