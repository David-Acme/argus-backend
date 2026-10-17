#!/usr/bin/env bash

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

boot_lock_acquire() {
  local stack="$1"
  if [ -n "${ARGUS_STACK_BOOT_LOCKED:-}" ]; then
    return 0
  fi
  need_cmd flock
  mkdir -p "$stack"
  exec 9>"$stack/boot.lock"
  if ! flock -n 9; then
    err "another boot of $stack is in flight, or its services still hold $stack/boot.lock: stop the stack (scripts/scratch-stack.sh --down, or ARGUS_STACK_DIR=$stack scripts/native-stack.sh down) before booting again"
    return 1
  fi
  export ARGUS_STACK_BOOT_LOCKED=1
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

toml_literal() {
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
      print value
      exit
    }' "$file"
}

toml_value() {
  local value
  value="$(toml_literal "$1" "$2" "$3")"
  if [[ "$value" == \"*\" ]]; then
    value="${value#\"}"
    value="${value%\"}"
  fi
  printf '%s\n' "$value"
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

TOML_VALUE_AWK='
  function toml_unquoted(line, key,   value) {
    value = line
    sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", value)
    sub("[[:space:]]+#.*$", "", value)
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
    if (value ~ /^".*"$/)
      value = substr(value, 2, length(value) - 2)
    return value
  }
  /^[[:space:]]*\[/ {
    in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
  }
'

toml_secret_set() {
  local file="$1"
  local table="$2"
  local key="$3"
  local min="${4:-1}"
  local pattern="${5:-}"

  [ -f "$file" ] || return 1
  awk -v table="$table" -v key="$key" -v min="$min" -v pattern="$pattern" "$TOML_VALUE_AWK"'
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      value = toml_unquoted($0, key)
      set = length(value) >= min + 0 && value !~ /CHANGE_ME/ && (pattern == "" || value ~ pattern)
      exit
    }
    END { exit !set }' "$file"
}

toml_secret_emit() {
  awk -v table="$2" -v key="$3" "$TOML_VALUE_AWK"'
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      print toml_unquoted($0, key)
      exit
    }' "$1"
}

toml_secrets_equal() {
  awk -v table="$2" -v first="$3" -v second="$4" "$TOML_VALUE_AWK"'
    in_table && $0 ~ "^[[:space:]]*" first "[[:space:]]*=" { a = toml_unquoted($0, first) }
    in_table && $0 ~ "^[[:space:]]*" second "[[:space:]]*=" { b = toml_unquoted($0, second) }
    END { exit !(b != "" && a == b) }' "$1"
}

toml_literal_is() {
  ARGUS_TOML_EXPECTED="$4" awk -v table="$2" -v key="$3" '
    /^[[:space:]]*\[/ {
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      value = $0
      sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", value)
      sub("[[:space:]]+#.*$", "", value)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
      same = (value == ENVIRON["ARGUS_TOML_EXPECTED"])
      exit
    }
    END { exit !same }' "$1"
}

toml_secret_generate() {
  openssl rand -hex "$4" | replace_toml_value_from_file "$2" "$3" /dev/stdin "$1"
}

toml_secret_copy() {
  toml_secret_emit "$1" "$2" "$3" | replace_toml_value_from_file "$5" "$6" /dev/stdin "$4"
}

replace_toml_value() {
  local table="$1"
  local key="$2"
  local value="$3"
  local config="$4"
  local mode="${5:-quoted}"
  local rendered

  if [ "$mode" = literal ]; then
    rendered="$value"
  else
    rendered="\"$value\""
  fi

  if ! grep -Eq "^[[:space:]]*\\[$table\\][[:space:]]*$" "$config"; then
    printf '\n[%s]\n%s = %s\n' "$table" "$key" "$rendered" >> "$config"
    return
  fi

  local temp
  temp="$(mktemp "${config}.tmp.XXXXXX")"
  ARGUS_TOML_RENDERED="$rendered" awk -v table="$table" -v key="$key" '
    BEGIN { rendered = ENVIRON["ARGUS_TOML_RENDERED"] }
    function emit() {
      if (in_table && !found) {
        print key " = " rendered
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
      print key " = " rendered
      found = 1
      next
    }
    { print }
    END { emit() }
  ' "$config" > "$temp"
  chmod 600 "$temp"
  mv "$temp" "$config"
}

replace_toml_value_from_file() {
  local table="$1"
  local key="$2"
  local source="$3"
  local config="$4"
  local temp

  temp="$(mktemp "${config}.tmp.XXXXXX")"
  if ! awk -v table="$table" -v key="$key" -v source="$source" '
    BEGIN {
      if ((getline value < source) <= 0) {
        failed = 1
        exit 2
      }
      sub(/\r$/, "", value)
      rendered = "\"" value "\""
    }
    function emit() {
      if (in_table && !found) {
        print key " = " rendered
        found = 1
      }
    }
    /^[[:space:]]*\[/ {
      emit()
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
      if (in_table)
        seen = 1
      found = 0
      print
      next
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      print key " = " rendered
      found = 1
      next
    }
    { print }
    END {
      if (failed)
        exit 2
      emit()
      if (!seen)
        printf "\n[%s]\n%s = %s\n", table, key, rendered
    }
  ' "$config" > "$temp"; then
    rm -f "$temp"
    return 1
  fi
  chmod 600 "$temp"
  mv "$temp" "$config"
}

carry_runtime_state() {
  local previous="$1"
  local config="$2"
  local paired owner push

  paired="$(toml_value "$previous" pairing paired)"
  if [ "$paired" = true ]; then
    owner="$(toml_value "$previous" pairing owner_device)"
    replace_toml_value pairing paired true "$config" literal
    replace_toml_value pairing owner_device "$owner" "$config"
  fi
  push="$(toml_value "$previous" push enabled)"
  if [ -n "$push" ]; then
    replace_toml_value push enabled "$push" "$config" literal
  fi
}

install_source_config() {
  local source="$1"
  local config="$2"
  local previous

  previous="$(mktemp)"
  if [ -f "$config" ]; then
    cp -p "$config" "$previous"
  fi
  install -m 600 "$source" "$config"
  if [ -s "$previous" ]; then
    carry_runtime_state "$previous" "$config"
  fi
  rm -f "$previous"
}

remove_toml_key() {
  local table="$1"
  local key="$2"
  local config="$3"
  local temp

  [ -f "$config" ] || return 0
  toml_key_exists "$config" "$table" "$key" || return 0
  temp="$(mktemp "${config}.tmp.XXXXXX")"
  awk -v table="$table" -v key="$key" '
    /^[[:space:]]*\[/ {
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" { next }
    { print }
  ' "$config" > "$temp"
  chmod 600 "$temp"
  mv "$temp" "$config"
}

propagate_config_value() {
  local source="$1"
  local table="$2"
  local key="$3"
  shift 3
  local value config

  [ -f "$source" ] || return 0
  toml_key_exists "$source" "$table" "$key" || return 0
  value="$(toml_value "$source" "$table" "$key")"
  [ -n "$value" ] || return 0
  for config in "$@"; do
    [ -f "$config" ] && [ "$config" != "$source" ] || continue
    toml_key_exists "$config" "$table" "$key" || continue
    [ "$(toml_value "$config" "$table" "$key")" = "$value" ] && continue
    replace_toml_value "$table" "$key" "$value" "$config"
  done
}

ensure_distinct_refresh_secret() {
  local config="$1"

  [ -f "$config" ] || return 0
  toml_key_exists "$config" jwt refresh_secret || return 0
  if toml_secrets_equal "$config" jwt secret refresh_secret; then
    toml_secret_generate "$config" jwt refresh_secret 48
    warn "jwt.refresh_secret equalled jwt.secret in $config; a new one was generated (refresh sessions end once)"
  fi
}

drop_verifier_refresh_secret() {
  local config
  for config in "$@"; do
    case "$config" in
      config.auth.toml|*/config.auth.toml|*/auth/config.toml) continue ;;
    esac
    remove_toml_key jwt refresh_secret "$config"
  done
}

ensure_tunnel_secret() {
  local first="$1"
  local second="${2:-}"
  local source="" config

  for config in "$first" "$second"; do
    [ -n "$config" ] && [ -f "$config" ] || continue
    toml_secret_set "$config" tunnel secret 32 || continue
    source="$config"
    break
  done
  for config in "$first" "$second"; do
    [ -n "$config" ] && [ -f "$config" ] && [ "$config" != "$source" ] || continue
    if [ -z "$source" ]; then
      toml_secret_generate "$config" tunnel secret 32
      source="$config"
    else
      toml_secret_copy "$source" tunnel secret "$config" tunnel secret
    fi
  done
}

ensure_link_identity() {
  local config="$1"
  local certs_dir="$2"
  local node_cert="$3"
  local host="${4:-argus-tunnel.lynk.page}"
  local port="${5:-7001}"
  local home_id certificate key

  [ -f "$config" ] || return 0
  mkdir -p "$certs_dir"
  chmod 700 "$certs_dir"
  home_id="$(toml_value "$config" link home_id)"
  case "$home_id" in
    ""|*[!0-9a-f]*) home_id="" ;;
  esac
  [ "${#home_id}" -eq 32 ] || home_id="$(openssl rand -hex 16)"

  certificate="$certs_dir/link.crt"
  key="$certs_dir/link.key"
  if [ ! -s "$certificate" ] || [ ! -s "$key" ]; then
    rm -f "$certificate" "$key"
    openssl req -x509 -newkey ed25519 -nodes -days 3650 \
      -keyout "$key" -out "$certificate" -subj "/CN=$home_id" >/dev/null 2>&1
    chmod 600 "$key"
    chmod 644 "$certificate"
  fi

  replace_toml_value link home_id "$home_id" "$config"
  replace_toml_value link certificate_path "$certificate" "$config"
  replace_toml_value link key_path "$key" "$config"
  replace_toml_value link host "$host" "$config"
  replace_toml_value link port "$port" "$config" literal
  if [ -n "$node_cert" ] && [ -s "$node_cert" ]; then
    replace_toml_value link node_certificate_path "$node_cert" "$config"
    replace_toml_value link enabled true "$config" literal
    log "Home link v2 on: home $home_id pins the cloud node certificate at $node_cert"
  else
    log "Home link v2 identity is at $certificate (home $home_id); place the cloud node certificate and set link.enabled to dial it"
  fi
}

ipv4_network_of() {
  local cidr="$1"
  local address="${cidr%/*}"
  local prefix="${cidr#*/}"
  local a b c d value mask

  case "$cidr" in */*) ;; *) return 1 ;; esac
  IFS=. read -r a b c d <<<"$address" || return 1
  for value in "$a" "$b" "$c" "$d" "$prefix"; do
    case "$value" in ""|*[!0-9]*) return 1 ;; esac
  done
  [ "$prefix" -le 32 ] || return 1
  value=$(( (a << 24) | (b << 16) | (c << 8) | d ))
  mask=$(( prefix == 0 ? 0 : (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF ))
  value=$(( value & mask ))
  printf '%d.%d.%d.%d/%d' $(( (value >> 24) & 255 )) $(( (value >> 16) & 255 )) \
    $(( (value >> 8) & 255 )) $(( value & 255 )) "$prefix"
}

host_lan_networks() {
  local address="$1"
  local cidr

  command -v ip >/dev/null 2>&1 || return 0
  [ -n "$address" ] || return 0
  cidr="$(ip -o -f inet addr show 2>/dev/null \
    | awk -v want="$address" '{ split($4, part, "/"); if (part[1] == want) { print $4; exit } }')"
  [ -n "$cidr" ] || return 0
  ipv4_network_of "$cidr"
}

configure_lan_networks() {
  local networks="$1"
  shift
  local config current

  [ -n "$networks" ] || return 0
  for config in "$@"; do
    [ -f "$config" ] || continue
    toml_key_exists "$config" device lan_networks || continue
    current="$(toml_value "$config" device lan_networks)"
    [ -z "$current" ] || continue
    replace_toml_value device lan_networks "$networks" "$config"
  done
}

adopt_template_key() {
  local template="$1"
  local table="$2"
  local key="$3"
  local config="$4"

  toml_key_exists "$template" "$table" "$key" || return 0

  local literal
  literal="$(toml_literal "$template" "$table" "$key")"

  if toml_key_exists "$config" "$table" "$key"; then
    toml_literal_is "$config" "$table" "$key" "\"$literal\"" || return 0
  fi

  replace_toml_value "$table" "$key" "$literal" "$config" literal
}

adopt_wiring_keys() {
  local template="$1"
  local config="$2"
  local table key

  while read -r table key; do
    adopt_template_key "$template" "$table" "$key" "$config"
  done <<'EOF'
device fingerprint_secret
device identity_mode
device trust_forwarded_for
device trusted_proxy_ips
auth target
auth credential
identity target
identity proxy_url
identity credential
camera actions_credential
notifications target
notifications credential
notification target
notification credential
productivity credential
voice target
voice credential
camera credential
sync control_target
sync control_credential
grpc caller_guard
grpc caller_sync
grpc caller_llm
grpc caller_settings
grpc caller_voice
grpc caller_productivity
grpc caller_notification
rpc.callers settings
rpc.callers sync
rpc.callers notification
rpc.callers voice
rpc.callers auth
rpc.callers camera
rpc.callers guard
rpc.callers identity
rpc.callers llm
rpc.callers productivity
llm grpc_credential
guard presence_target
guard presence_credential
modules target
modules credential
nats url
nats nkey
nats seed_file
nats inbox_prefix
rpc address
internal_tls dir
internal_tls address
owners.llm target
owners.llm credential
owners.llm config_file
owners.voice target
owners.voice credential
owners.voice config_file
owners.tts target
owners.tts credential
owners.tts config_file
owners.stt target
owners.stt credential
owners.stt config_file
owners.vlm target
owners.vlm credential
owners.vlm config_file
owners.guard target
owners.guard credential
owners.guard config_file
owners.camera target
owners.camera credential
owners.camera config_file
owners.notification target
owners.notification credential
owners.notification config_file
owners.identity target
owners.identity credential
owners.productivity target
owners.productivity credential
owners.sync target
owners.sync credential
settings profiles_path
mdns enabled
EOF
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
  adopt_wiring_keys "$template" "$config"
  drop_verifier_refresh_secret "$config"
  chmod 600 "$config"
}

share_config_value() {
  local table="$1"
  local key="$2"
  local bytes="$3"
  shift 3

  local config source=""
  for config in "$@"; do
    toml_secret_set "$config" "$table" "$key" || continue
    source="$config"
    break
  done
  for config in "$@"; do
    [ "$config" != "$source" ] || continue
    toml_key_exists "$config" "$table" "$key" || continue
    if [ -z "$source" ]; then
      toml_secret_generate "$config" "$table" "$key" "$bytes"
      source="$config"
    else
      toml_secret_copy "$source" "$table" "$key" "$config" "$table" "$key"
    fi
  done
}

ensure_shared_configs() {
  local configs=("$@")

  share_config_value jwt secret 48 "${configs[@]}"
  share_config_value jwt refresh_secret 48 "${configs[@]}"
  share_config_value device fingerprint_secret 48 "${configs[@]}"
  share_config_value identity rpc_secret 32 "${configs[@]}"
  local config
  for config in "${configs[@]}"; do
    ensure_distinct_refresh_secret "$config"
  done
}

share_deploy_secret() {
  local deploy_dir="$1"
  local table="$2"
  local key="$3"
  local bytes="$4"
  local config source=""

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    toml_secret_set "$config" "$table" "$key" || continue
    source="$config"
    break
  done
  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] && [ "$config" != "$source" ] || continue
    toml_key_exists "$config" "$table" "$key" || continue
    toml_secret_set "$config" "$table" "$key" && continue
    if [ -z "$source" ]; then
      toml_secret_generate "$config" "$table" "$key" "$bytes"
      source="$config"
    else
      toml_secret_copy "$source" "$table" "$key" "$config" "$table" "$key"
    fi
  done
}

fill_config_pair() {
  local a_config="$1"
  local a_table="$2"
  local a_key="$3"
  local b_config="$4"
  local b_table="$5"
  local b_key="$6"
  local bytes="$7"

  [ -f "$a_config" ] && [ -f "$b_config" ] || return 0
  toml_key_exists "$a_config" "$a_table" "$a_key" || return 0
  toml_key_exists "$b_config" "$b_table" "$b_key" || return 0
  if toml_secret_set "$a_config" "$a_table" "$a_key"; then
    toml_secret_copy "$a_config" "$a_table" "$a_key" "$b_config" "$b_table" "$b_key"
  elif toml_secret_set "$b_config" "$b_table" "$b_key"; then
    toml_secret_copy "$b_config" "$b_table" "$b_key" "$a_config" "$a_table" "$a_key"
  else
    toml_secret_generate "$a_config" "$a_table" "$a_key" "$bytes"
    toml_secret_copy "$a_config" "$a_table" "$a_key" "$b_config" "$b_table" "$b_key"
  fi
}

fleet_caller_config() {
  local mode="$1"
  local base="$2"
  local service="$3"

  case "$mode" in
    deploy) printf '%s/config.%s.toml' "$base" "$service" ;;
    stack) printf '%s/%s/config.toml' "$base" "$service" ;;
    *) printf '%s/services/%s/config.toml' "$base" "$service" ;;
  esac
}

ensure_fleet_callers() {
  local mode="$1"
  local base="$2"
  local caller table key server

  while read -r caller table key server; do
    fill_config_pair "$(fleet_caller_config "$mode" "$base" "$caller")" "$table" "$key" \
      "$(fleet_caller_config "$mode" "$base" "$server")" rpc.callers "$caller" 32
  done <<'CALLERS'
auth identity credential identity
camera identity credential identity
guard identity credential identity
llm identity credential identity
notification identity credential identity
productivity identity credential identity
sync identity credential identity
voice identity credential identity
camera auth credential auth
guard auth credential auth
identity auth credential auth
notification auth credential auth
productivity auth credential auth
settings auth credential auth
sync auth credential auth
identity sync control_credential sync
notification sync control_credential sync
auth modules credential settings
camera modules credential settings
guard modules credential settings
identity modules credential settings
notification modules credential settings
productivity modules credential settings
sync modules credential settings
voice modules credential settings
llm modules credential settings
llm guard credential guard
CALLERS
}

settings_owner_port() {
  local config="$1"
  local listener="$2"
  local address

  case "$listener" in
    rpc)
      address="$(toml_value "$config" rpc address)"
      case "$address" in
        *:*) printf '%s' "${address##*:}" ;;
      esac
      ;;
    grpc)
      toml_value "$config" server grpc_port
      ;;
  esac
}

settings_owner_tls_port() {
  local config="$1"
  local address

  address="$(toml_value "$config" internal_tls address)"
  case "$address" in
    *:*) printf '%s' "${address##*:}" ;;
  esac
}

settings_owner_template() {
  local mode="$1"
  local base="$2"
  local owner="$3"

  if [ "$mode" = deploy ]; then
    printf '%s/config.%s.toml.example' "$base" "$owner"
  else
    printf '%s/services/%s/config.toml.example' "$ROOT" "$owner"
  fi
}

ensure_settings_owners() {
  local settings_config="$1"
  local mode="$2"
  local base="$3"
  local owner table key listener owner_config host port plain template address

  [ -f "$settings_config" ] || return 0
  while read -r owner table key listener; do
    case "$mode" in
      deploy)
        owner_config="$base/config.$owner.toml"
        host="argus-$owner"
        ;;
      stack)
        owner_config="$base/$owner/config.toml"
        host="127.0.0.1"
        ;;
      *)
        owner_config="$base/services/$owner/config.toml"
        host="127.0.0.1"
        ;;
    esac
    [ -f "$owner_config" ] || continue
    toml_key_exists "$owner_config" "$table" "$key" || continue
    toml_key_exists "$settings_config" "owners.$owner" credential || continue
    fill_config_pair "$settings_config" "owners.$owner" credential \
      "$owner_config" "$table" "$key" 32
    if [ "$mode" = deploy ] && toml_key_exists "$settings_config" "owners.$owner" config_file &&
      [ -z "$(toml_value "$settings_config" "owners.$owner" config_file)" ]; then
      replace_toml_value "owners.$owner" config_file "$owner_config" "$settings_config"
    fi
    template="$(settings_owner_template "$mode" "$base" "$owner")"
    if [ "$listener" = rpc ] && [ -f "$template" ] &&
      [ -z "$(toml_value "$owner_config" rpc address)" ]; then
      address="$(toml_value "$template" rpc address)"
      [ -z "$address" ] || replace_toml_value rpc address "$address" "$owner_config"
    fi
    [ -z "$(toml_value "$settings_config" "owners.$owner" target)" ] || continue
    plain="$(settings_owner_port "$owner_config" "$listener")"
    if [ -z "$plain" ] && [ -f "$template" ]; then
      plain="$(settings_owner_port "$template" "$listener")"
    fi
    [ -n "$plain" ] || continue
    port="$(settings_owner_tls_port "$owner_config")"
    if [ -z "$port" ] && [ -f "$template" ]; then
      port="$(settings_owner_tls_port "$template")"
    fi
    [ -n "$port" ] || continue
    replace_toml_value "owners.$owner" target "tls:$host:$port" "$settings_config"
  done <<'OWNERS'
llm rpc.callers settings rpc
voice grpc caller_settings grpc
tts rpc.callers settings rpc
stt rpc.callers settings rpc
vlm rpc.callers settings rpc
guard rpc.callers settings rpc
camera grpc caller_settings grpc
notification grpc caller_settings grpc
identity rpc.callers settings grpc
productivity grpc caller_settings grpc
sync rpc.callers settings grpc
OWNERS
}

ensure_livekit_key_pair() {
  local config="$1"
  [ -f "$config" ] || return 0
  toml_key_exists "$config" rtc api_key || return 0
  if toml_secret_set "$config" rtc api_key && toml_secret_set "$config" rtc api_secret 32; then
    return 0
  fi
  { printf 'argus'; openssl rand -hex 6; } | replace_toml_value_from_file rtc api_key /dev/stdin "$config"
  toml_secret_generate "$config" rtc api_secret 24
}

write_livekit_keys() {
  local config="$1"
  local keys="$2"
  if ! toml_secret_set "$config" rtc api_key || ! toml_secret_set "$config" rtc api_secret; then
    err "no [rtc] key pair in $config"
    return 1
  fi
  mkdir -p "$(dirname "$keys")"
  (umask 077 && {
    toml_secret_emit "$config" rtc api_key | tr -d '\n'
    printf ': '
    toml_secret_emit "$config" rtc api_secret
  } > "$keys")
  chmod 600 "$keys"
}

retire_obsolete_deploy_keys() {
  local deploy_dir="$1"
  local config table key

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    while read -r table key; do
      [ -n "$key" ] || continue
      remove_toml_key "$table" "$key" "$config"
    done <<'RETIRED'
nats user
nats password
RETIRED
  done
}

ensure_deploy_configs() {
  local deploy_dir="$1"
  local template config

  [ -d "$deploy_dir" ] || { err "missing deploy dir: $deploy_dir"; return 1; }
  retire_obsolete_deploy_keys "$deploy_dir"

  for template in "$deploy_dir"/config.*.toml.example; do
    [ -f "$template" ] || continue
    config="${template%.example}"
    if [ ! -f "$config" ]; then
      umask 077
      cp "$template" "$config"
    fi
    adopt_wiring_keys "$template" "$config"
    chmod 600 "$config"
  done

  local table key bytes
  while read -r table key bytes; do
    share_deploy_secret "$deploy_dir" "$table" "$key" "$bytes"
  done <<'SECRETS'
jwt secret 48
jwt refresh_secret 48
device fingerprint_secret 48
identity rpc_secret 32
auth rpc_secret 32
sync control_secret 32
SECRETS

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    drop_verifier_refresh_secret "$config"
    ensure_distinct_refresh_secret "$config"
  done
  propagate_config_value "$deploy_dir/config.auth.toml" device identity_mode \
    "$deploy_dir"/config.*.toml

  fill_config_pair "$deploy_dir/config.guard.toml" camera actions_credential \
    "$deploy_dir/config.camera.toml" grpc caller_guard 32
  fill_config_pair "$deploy_dir/config.guard.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_guard 32
  fill_config_pair "$deploy_dir/config.productivity.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_productivity 32
  fill_config_pair "$deploy_dir/config.llm.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_llm 32
  fill_config_pair "$deploy_dir/config.voice.toml" notification credential \
    "$deploy_dir/config.notification.toml" grpc caller_voice 32
  fill_config_pair "$deploy_dir/config.notification.toml" voice credential \
    "$deploy_dir/config.voice.toml" grpc caller_notification 32
  fill_config_pair "$deploy_dir/config.camera.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_camera 32
  fill_config_pair "$deploy_dir/config.sync.toml" notifications credential \
    "$deploy_dir/config.notification.toml" grpc caller_sync 32
  fill_config_pair "$deploy_dir/config.sync.toml" productivity credential \
    "$deploy_dir/config.productivity.toml" grpc caller_sync 32
  fill_config_pair "$deploy_dir/config.sync.toml" voice credential \
    "$deploy_dir/config.voice.toml" grpc caller_sync 32
  fill_config_pair "$deploy_dir/config.sync.toml" camera credential \
    "$deploy_dir/config.camera.toml" grpc caller_sync 32
  fill_config_pair "$deploy_dir/config.llm.toml" camera credential \
    "$deploy_dir/config.camera.toml" grpc caller_llm 32
  fill_config_pair "$deploy_dir/config.llm.toml" productivity credential \
    "$deploy_dir/config.productivity.toml" grpc caller_llm 32
  fill_config_pair "$deploy_dir/config.voice.toml" stt grpc_credential \
    "$deploy_dir/config.stt.toml" rpc.callers voice 32
  fill_config_pair "$deploy_dir/config.voice.toml" tts grpc_credential \
    "$deploy_dir/config.tts.toml" rpc.callers voice 32
  fill_config_pair "$deploy_dir/config.voice.toml" llm grpc_credential \
    "$deploy_dir/config.llm.toml" rpc.callers voice 32
  fill_config_pair "$deploy_dir/config.camera.toml" stt grpc_credential \
    "$deploy_dir/config.stt.toml" rpc.callers camera 32
  fill_config_pair "$deploy_dir/config.camera.toml" tts grpc_credential \
    "$deploy_dir/config.tts.toml" rpc.callers camera 32
  fill_config_pair "$deploy_dir/config.guard.toml" vlm grpc_credential \
    "$deploy_dir/config.vlm.toml" rpc.callers guard 32
  fill_config_pair "$deploy_dir/config.guard.toml" llm grpc_credential \
    "$deploy_dir/config.llm.toml" rpc.callers guard 32
  fill_config_pair "$deploy_dir/config.sync.toml" guard presence_credential \
    "$deploy_dir/config.guard.toml" rpc.callers sync 32
  fill_config_pair "$deploy_dir/config.notification.toml" guard presence_credential \
    "$deploy_dir/config.guard.toml" rpc.callers notification 32
  ensure_fleet_callers deploy "$deploy_dir"
  ensure_settings_owners "$deploy_dir/config.settings.toml" deploy "$deploy_dir"
  ensure_livekit_key_pair "$deploy_dir/config.sync.toml"
  if [ -f "$deploy_dir/config.sync.toml" ]; then
    write_livekit_keys "$deploy_dir/config.sync.toml" "$deploy_dir/livekit-keys.yaml"
  fi
  if [ ! -f "$deploy_dir/livekit.yaml" ] && [ -f "$deploy_dir/livekit.yaml.example" ]; then
    cp "$deploy_dir/livekit.yaml.example" "$deploy_dir/livekit.yaml"
  fi

  log "Deploy configs ready in $deploy_dir"
}
