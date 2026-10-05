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
  local access refresh

  [ -f "$config" ] || return 0
  toml_key_exists "$config" jwt refresh_secret || return 0
  access="$(toml_value "$config" jwt secret)"
  refresh="$(toml_value "$config" jwt refresh_secret)"
  if [ -n "$refresh" ] && [ "$refresh" = "$access" ]; then
    replace_toml_value jwt refresh_secret "$(openssl rand -hex 48)" "$config"
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
  local value="" candidate config

  for config in "$first" "$second"; do
    [ -n "$config" ] && [ -f "$config" ] || continue
    toml_key_exists "$config" tunnel secret || continue
    candidate="$(toml_value "$config" tunnel secret)"
    case "$candidate" in
      ""|*CHANGE_ME*) continue ;;
    esac
    [ "${#candidate}" -ge 32 ] || continue
    value="$candidate"
    break
  done
  [ -n "$value" ] || value="$(openssl rand -hex 32)"
  for config in "$first" "$second"; do
    [ -n "$config" ] && [ -f "$config" ] || continue
    replace_toml_value tunnel secret "$value" "$config"
  done
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
    [ "$(toml_literal "$config" "$table" "$key")" = "\"$literal\"" ] || return 0
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
auth rpc_host
auth rpc_port
auth rpc_secret
identity target
identity proxy_url
identity rpc_host
identity rpc_port
identity rpc_secret
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
sync control_secret
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
guard presence_target
guard presence_credential
rpc address
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

shared_config_value() {
  local table="$1"
  local key="$2"
  local bytes="$3"
  shift 3

  local config value
  for config in "$@"; do
    toml_key_exists "$config" "$table" "$key" || continue
    value="$(toml_value "$config" "$table" "$key")"
    if [ -n "$value" ]; then
      printf '%s' "$value"
      return 0
    fi
  done
  openssl rand -hex "$bytes"
}

share_config_value() {
  local table="$1"
  local key="$2"
  local bytes="$3"
  shift 3

  local value config
  value="$(shared_config_value "$table" "$key" "$bytes" "$@")"
  for config in "$@"; do
    toml_key_exists "$config" "$table" "$key" || continue
    replace_toml_value "$table" "$key" "$value" "$config"
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
  local owner table key listener owner_config host port template address

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
    port="$(settings_owner_port "$owner_config" "$listener")"
    if [ -z "$port" ] && [ -f "$template" ]; then
      port="$(settings_owner_port "$template" "$listener")"
    fi
    [ -n "$port" ] || continue
    replace_toml_value "owners.$owner" target "$host:$port" "$settings_config"
  done <<'OWNERS'
llm rpc.callers settings rpc
voice grpc caller_settings grpc
tts rpc.callers settings rpc
stt rpc.callers settings rpc
vlm rpc.callers settings rpc
guard rpc.callers settings rpc
camera grpc caller_settings grpc
notification grpc caller_settings grpc
OWNERS
}

ensure_livekit_key_pair() {
  local config="$1"
  [ -f "$config" ] || return 0
  toml_key_exists "$config" rtc api_key || return 0
  local key secret
  key="$(toml_value "$config" rtc api_key)"
  secret="$(toml_value "$config" rtc api_secret)"
  case "$key" in ""|*CHANGE_ME*) key="" ;; esac
  case "$secret" in ""|*CHANGE_ME*) secret="" ;; esac
  if [ -n "$key" ] && [ "${#secret}" -ge 32 ]; then
    return 0
  fi
  replace_toml_value rtc api_key "argus$(openssl rand -hex 6)" "$config"
  replace_toml_value rtc api_secret "$(openssl rand -hex 24)" "$config"
}

write_livekit_keys() {
  local config="$1"
  local keys="$2"
  local key secret
  key="$(toml_value "$config" rtc api_key)"
  secret="$(toml_value "$config" rtc api_secret)"
  [ -n "$key" ] && [ -n "$secret" ] || { err "no [rtc] key pair in $config"; return 1; }
  mkdir -p "$(dirname "$keys")"
  (umask 077 && printf '%s: %s\n' "$key" "$secret" > "$keys")
  chmod 600 "$keys"
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
    adopt_wiring_keys "$template" "$config"
    chmod 600 "$config"
  done

  local jwt_secret refresh_secret fingerprint_secret rpc_secret auth_rpc_secret
  local control_secret nats_password
  jwt_secret="$(shared_deploy_secret "$deploy_dir" jwt secret 48)"
  refresh_secret="$(shared_deploy_secret "$deploy_dir" jwt refresh_secret 48)"
  fingerprint_secret="$(shared_deploy_secret "$deploy_dir" device fingerprint_secret 48)"
  rpc_secret="$(shared_deploy_secret "$deploy_dir" identity rpc_secret 32)"
  auth_rpc_secret="$(shared_deploy_secret "$deploy_dir" auth rpc_secret 32)"
  control_secret="$(shared_deploy_secret "$deploy_dir" sync control_secret 32)"
  nats_password="$(shared_deploy_secret "$deploy_dir" nats password 32)"

  for config in "$deploy_dir"/config.*.toml; do
    [ -f "$config" ] || continue
    fill_deploy_placeholder "$config" jwt secret "$jwt_secret"
    fill_deploy_placeholder "$config" jwt refresh_secret "$refresh_secret"
    fill_deploy_placeholder "$config" device fingerprint_secret "$fingerprint_secret"
    fill_deploy_placeholder "$config" identity rpc_secret "$rpc_secret"
    fill_deploy_placeholder "$config" auth rpc_secret "$auth_rpc_secret"
    fill_deploy_placeholder "$config" sync control_secret "$control_secret"
    fill_deploy_placeholder "$config" nats password "$nats_password"
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
