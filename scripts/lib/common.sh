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
