#!/usr/bin/env bash
# Shared shell helpers for the setup and per-project provision scripts.

set -euo pipefail

log()  { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    err "required command not found: $1"
    exit 1
  fi
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    return 1
  fi
}

sudo_if_needed() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    command -v sudo >/dev/null 2>&1 && sudo "$@" || "$@"
  fi
}

toml_value() {
  local file="$1"
  local table="$2"
  local key="$3"
  awk -v table="$table" -v key="$key" '
    /^[[:space:]]*\[/ {
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      value = $0
      sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", value)
      sub("[[:space:]]+#.*$", "", value)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
      if (value ~ /^".*"$/) {
        sub(/^"/, "", value)
        sub(/"$/, "", value)
      }
      print value
      exit
    }' "$file"
}

toml_key_exists() {
  local file="$1"
  local table="$2"
  local key="$3"
  awk -v table="$table" -v key="$key" '
    /^[[:space:]]*\[/ {
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      found = 1
      exit
    }
    END { exit !found }' "$file"
}

replace_toml_value() {
  local table="$1"
  local key="$2"
  local value="$3"
  local config="$4"

  if ! grep -Eq "^[[:space:]]*\\[$table\\][[:space:]]*$" "$config"; then
    printf '\n[%s]\n%s = "%s"\n' "$table" "$key" "$value" >> "$config"
    return
  fi

  local temp
  temp="$(mktemp "${config}.tmp.XXXXXX")"
  awk -v table="$table" -v key="$key" -v value="$value" '
    function emit() {
      if (in_table && !found) {
        print key " = \"" value "\""
        found = 1
      }
    }
    /^[[:space:]]*\[/ {
      emit()
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
      found = 0
      print
      next
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      print key " = \"" value "\""
      found = 1
      next
    }
    { print }
    END { emit() }
  ' "$config" > "$temp"
  chmod 600 "$temp"
  mv "$temp" "$config"
}

ensure_toml_value() {
  local table="$1"
  local key="$2"
  local value="$3"
  local config="$4"
  local min_length="${5:-1}"
  local max_length="${6:-0}"

  if ! grep -Eq "^[[:space:]]*\\[$table\\][[:space:]]*$" "$config"; then
    printf '\n[%s]\n%s = "%s"\n' "$table" "$key" "$value" >> "$config"
    return
  fi

  local existing
  existing="$(toml_value "$config" "$table" "$key")"
  if [ "${#existing}" -ge "$min_length" ] &&
     { [ "$max_length" -eq 0 ] || [ "${#existing}" -le "$max_length" ]; }; then
    return
  fi

  replace_toml_value "$table" "$key" "$value" "$config"
}

ensure_project_config() {
  local dir="$1"
  local template="$dir/config.toml.example"
  local config="$dir/config.toml"

  [ -f "$template" ] || { err "missing config template: $template"; return 1; }

  if [ ! -f "$config" ]; then
    umask 077
    cp "$template" "$config"
  fi
  chmod 600 "$config"

  local table key bytes value
  while read -r table key bytes; do
    toml_key_exists "$template" "$table" "$key" || continue
    value="$(openssl rand -hex "$bytes")"
    ensure_toml_value "$table" "$key" "$value" "$config"
  done <<'EOF'
jwt secret 48
jwt refresh_secret 48
device fingerprint_secret 48
identity rpc_secret 32
EOF
}

shared_deploy_secret() {
  local deploy_dir="$1"
  local table="$2"
  local key="$3"
  local bytes="$4"
  local config value

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    toml_key_exists "$config" "$table" "$key" || continue
    value="$(toml_value "$config" "$table" "$key")"
    case "$value" in
      ""|*CHANGE_ME*) continue ;;
      *) printf '%s' "$value"; return 0 ;;
    esac
  done
  openssl rand -hex "$bytes"
}

# Fills the single capability secret of one directed edge into both of its
# configs, reusing whichever side already holds a real value.
fill_deploy_pair() {
  local a_config="$1"
  local a_table="$2"
  local a_key="$3"
  local b_config="$4"
  local b_table="$5"
  local b_key="$6"
  local bytes="$7"

  [ -f "$a_config" ] && [ -f "$b_config" ] || return 0
  local value="" candidate
  candidate="$(toml_value "$a_config" "$a_table" "$a_key")"
  case "$candidate" in ""|*CHANGE_ME*) ;; *) value="$candidate" ;; esac
  if [ -z "$value" ]; then
    candidate="$(toml_value "$b_config" "$b_table" "$b_key")"
    case "$candidate" in ""|*CHANGE_ME*) ;; *) value="$candidate" ;; esac
  fi
  [ -n "$value" ] || value="$(openssl rand -hex "$bytes")"
  replace_toml_value "$a_table" "$a_key" "$value" "$a_config"
  replace_toml_value "$b_table" "$b_key" "$value" "$b_config"
}

fill_deploy_placeholder() {
  local config="$1"
  local table="$2"
  local key="$3"
  local value="$4"
  local current

  toml_key_exists "$config" "$table" "$key" || return 0
  current="$(toml_value "$config" "$table" "$key")"
  case "$current" in
    ""|*CHANGE_ME*) replace_toml_value "$table" "$key" "$value" "$config" ;;
  esac
}

# Copies argus-deploy/config.<name>.toml.example into the gitignored 0600
# instance files and fills the shared instance secrets. Updates reuse the
# first existing value for each secret, so tokens, device hashes and the
# identity RPC secret survive every image update.
ensure_deploy_configs() {
  local deploy_dir="$1"
  local template config

  [ -d "$deploy_dir" ] || { err "missing deploy dir: $deploy_dir"; return 1; }

  for template in "$deploy_dir"/config.*.toml.example; do
    [ -f "$template" ] || continue
    config="${template%.example}"
    if [ ! -f "$config" ]; then
      umask 077
      cp "$template" "$config"
    fi
    chmod 600 "$config"
  done

  local jwt_secret refresh_secret fingerprint_secret rpc_secret
  jwt_secret="$(shared_deploy_secret "$deploy_dir" jwt secret 48)"
  refresh_secret="$(shared_deploy_secret "$deploy_dir" jwt refresh_secret 48)"
  fingerprint_secret="$(shared_deploy_secret "$deploy_dir" device fingerprint_secret 48)"
  rpc_secret="$(shared_deploy_secret "$deploy_dir" identity rpc_secret 32)"

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    fill_deploy_placeholder "$config" jwt secret "$jwt_secret"
    fill_deploy_placeholder "$config" jwt refresh_secret "$refresh_secret"
    fill_deploy_placeholder "$config" device fingerprint_secret "$fingerprint_secret"
    fill_deploy_placeholder "$config" identity rpc_secret "$rpc_secret"
    fill_deploy_placeholder "$config" device trusted_proxy_ips "172.19.0.1"
  done

  fill_deploy_pair "$deploy_dir/config.guard.toml" camera actions_credential \
    "$deploy_dir/config.camera.toml" grpc caller_guard 32
  fill_deploy_pair "$deploy_dir/config.guard.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_guard 32
  fill_deploy_pair "$deploy_dir/config.gateway.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_gateway 32

  log "Deploy configs ready in $deploy_dir"
}
