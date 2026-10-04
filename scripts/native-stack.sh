#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"

STACK_DIR="${ARGUS_STACK_DIR:-$ROOT/build/native-stack}"
case "$STACK_DIR" in
  /*) ;;
  *) STACK_DIR="$ROOT/$STACK_DIR" ;;
esac
PROFILE="${ARGUS_STACK_PROFILE:-dev}"
SERVICES=(identity auth camera productivity notification sync guard settings)
MDNS_ENABLED=false
case "${ARGUS_STACK_MDNS:-0}" in
  1 | true | on | yes) MDNS_ENABLED=true ;;
esac
declare -A SECTION=(
  [identity]=identity
  [auth]=auth
  [camera]=camera
  [productivity]=productivity
  [notification]=notifications
  [sync]=sync
  [guard]=database
  [settings]=
)

usage() {
  cat <<'USAGE'
Argus backend - native stack runner.

Boots the eight request-serving services (identity, auth, camera,
productivity, notification, sync, guard, settings) natively, each with its
own database under ARGUS_STACK_DIR (settings owns none), so a golden replay
can run against a real fleet without touching the developer's own databases.
The sandbox's settings service reaches only owners the sandbox boots, so
every other owner's target is emptied in its copy of the config. Guard,
camera and notification are paired with it (settings credential and target)
on prepare and on every restart of settings or of one of them; restart
settings after a restart that minted a new pair. The sandbox's settings
service never applies the recommended profile on its own (first_run =
false), so a replay finds every owner's values as the copied config left
them.

The sandbox claims the standard ports (7025-7045, plus guard's settings
listener on 7139). Stop any other native
run of these services before `up`; the docker compose stack may stay up
for nats, rustfs, voice, stt, tts and vlm.

Usage:
  native-stack.sh up                prepare configs, start, gate on /health
  native-stack.sh down              stop the stack and its go2rtc
  native-stack.sh restart <service> stop and start one service, preparing its
                                    config first when the sandbox has none
  native-stack.sh kill <service>    stop one service (durability drills)
  native-stack.sh sigkill <service> kill one service outright, no drain: the
                                    process dies where it stands, mid-write
  native-stack.sh freeze <service>  hold one service with SIGSTOP, sockets
                                    open and state frozen; release it with
                                    sigkill
  native-stack.sh status            show what is running
  native-stack.sh logs <service> [n]  tail one service's log
  native-stack.sh prepare           write the sandbox configs only
  native-stack.sh env               mint a session and print the harness
                                    environment, for eval "$(native-stack.sh
                                    env)" before a golden replay

Environment:
  ARGUS_STACK_DIR      sandbox directory (default build/native-stack)
  ARGUS_STACK_PROFILE  dev or prod (default dev)
  ARGUS_STACK_MDNS     1 makes every service advertise its routes over mDNS
                       (mdns.enabled=true, mdns.address emptied so each
                       responder enumerates this host's interfaces)

A service started here outlives the command that started it, and with it any
descriptor it inherited. Wrap a stack run in a lock with `flock -o FILE ...`:
plain `flock FILE native-stack.sh up` leaves the lock held by the services,
and it is released only when the last of them stops.
USAGE
}

binary_of() {
  printf '%s/services/%s/build/%s/argus-%s' "$ROOT" "$1" "$PROFILE" "$1"
}

config_of() {
  printf '%s/%s/config.toml' "$STACK_DIR" "$1"
}

token_file() {
  printf '%s/refresh-token' "$STACK_DIR"
}

replace_top_key() {
  local key="$1"
  local value="$2"
  local config="$3"
  local temp

  temp="$(mktemp "${config}.tmp.XXXXXX")"
  awk -v key="$key" -v value="$value" '
    !done && $0 ~ "^" key "[[:space:]]*=" {
      print key " = \"" value "\""
      done = 1
      next
    }
    { print }
    END { if (!done) print key " = \"" value "\"" }
  ' "$config" > "$temp"
  chmod 600 "$temp"
  mv "$temp" "$config"
}

service_port() {
  awk -F'[ =]+' '$1 == "port" && $2 ~ /^[0-9]+$/ { print $2; exit }' "$1"
}

stack_service() {
  local known
  for known in "${SERVICES[@]}"; do
    [ "$known" = "$1" ] && return 0
  done
  return 1
}

isolate_settings_owners() {
  local config="$1"
  local owner
  for owner in $(awk '/^\[owners\.[a-z]+\][[:space:]]*$/ {
      name = $0
      sub(/^\[owners\./, "", name)
      sub(/\].*$/, "", name)
      print name
    }' "$config"); do
    stack_service "$owner" && continue
    replace_toml_value "owners.$owner" target "" "$config"
  done
}

prepare_service() {
  local svc="$1"
  local template="$ROOT/services/$svc/config.toml.example"
  local source_config="$ROOT/services/$svc/config.toml"
  local config
  config="$(config_of "$svc")"

  [ -f "$source_config" ] || {
    err "missing $source_config; run scripts/setup.sh first"
    return 1
  }

  mkdir -p "$STACK_DIR/$svc/database" "$STACK_DIR/logs" "$STACK_DIR/pids"
  install -m 600 "$source_config" "$config"
  adopt_wiring_keys "$template" "$config"

  if [ -n "${SECTION[$svc]}" ]; then
    local db schema
    db="$(toml_value "$config" "${SECTION[$svc]}" db)"
    case "$db" in
      /*)
        err "$source_config uses the absolute db path $db; a sandbox needs a relative one"
        return 1
        ;;
    esac
    replace_top_key db "$STACK_DIR/$svc/$db" "$config"

    schema="$(toml_value "$config" "${SECTION[$svc]}" schema)"
    case "$schema" in
      /*) ;;
      *) replace_top_key schema "$ROOT/$schema" "$config" ;;
    esac
  fi

  replace_toml_value mdns enabled "$MDNS_ENABLED" "$config" literal
  if [ "$MDNS_ENABLED" = true ]; then
    replace_toml_value mdns address "" "$config"
  fi
  ln -sfn "$ROOT/certs" "$STACK_DIR/$svc/certs"

  if [ "$svc" = identity ]; then
    ln -sfn "$ROOT/models" "$STACK_DIR/identity/models"
  fi

  if [ "$svc" = settings ]; then
    isolate_settings_owners "$config"
    replace_toml_value settings profiles_path "$ROOT/services/settings/profiles.json" "$config"
    replace_toml_value settings first_run false "$config" literal
  fi

  if [ "$svc" = camera ]; then
    replace_top_key go2rtc_bin "$ROOT/third_party/go2rtc/go2rtc" "$config"
    replace_top_key go2rtc_config "$STACK_DIR/camera/go2rtc.yaml" "$config"
    : > "$STACK_DIR/camera/go2rtc.yaml"
  fi

  log "prepared $config"
}

settings_owner() {
  case "$1" in
    settings | guard | camera | notification) return 0 ;;
  esac
  return 1
}

wire_settings_owners() {
  local settings_config owner config
  settings_config="$(config_of settings)"
  [ -f "$settings_config" ] || return 0
  for owner in guard camera notification; do
    config="$(config_of "$owner")"
    [ -f "$config" ] || continue
    adopt_wiring_keys "$ROOT/services/$owner/config.toml.example" "$config"
  done
  ensure_settings_owners "$settings_config" stack "$STACK_DIR"
}

ensure_prepared() {
  [ -f "$(config_of "$1")" ] || prepare_service "$1"
}

prepare() {
  local svc
  for svc in "${SERVICES[@]}"; do
    prepare_service "$svc" || return 1
  done

  fill_config_pair "$STACK_DIR/sync/config.toml" notifications credential \
    "$STACK_DIR/notification/config.toml" grpc caller_sync 32
  fill_config_pair "$STACK_DIR/sync/config.toml" productivity credential \
    "$STACK_DIR/productivity/config.toml" grpc caller_sync 32
  fill_config_pair "$STACK_DIR/sync/config.toml" voice credential \
    "$STACK_DIR/voice/config.toml" grpc caller_sync 32
  fill_config_pair "$STACK_DIR/sync/config.toml" camera credential \
    "$STACK_DIR/camera/config.toml" grpc caller_sync 32
  fill_config_pair "$STACK_DIR/llm/config.toml" camera credential \
    "$STACK_DIR/camera/config.toml" grpc caller_llm 32
  fill_config_pair "$STACK_DIR/productivity/config.toml" notifications credential \
    "$STACK_DIR/notification/config.toml" grpc caller_productivity 32
  fill_config_pair "$STACK_DIR/llm/config.toml" notifications credential \
    "$STACK_DIR/notification/config.toml" grpc caller_llm 32
  fill_config_pair "$STACK_DIR/voice/config.toml" notification credential \
    "$STACK_DIR/notification/config.toml" grpc caller_voice 32
  fill_config_pair "$STACK_DIR/notification/config.toml" voice credential \
    "$STACK_DIR/voice/config.toml" grpc caller_notification 32
  wire_settings_owners
  log "sandbox configs ready in $STACK_DIR"
}

running_pid() {
  pgrep -f "^$(binary_of "$1")\$" || true
}

wait_health() {
  local svc="$1"
  local port="$2"
  local attempt

  for attempt in $(seq 1 45); do
    if curl -sk -m 2 -o /dev/null "https://127.0.0.1:$port/health"; then
      log "$svc healthy on $port"
      return 0
    fi
    if [ -z "$(running_pid "$svc")" ]; then
      err "$svc exited before becoming healthy; see $STACK_DIR/logs/$svc.log"
      return 1
    fi
    sleep 1
  done
  err "$svc did not become healthy on $port"
  return 1
}

port_busy() {
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && exec 3>&- && return 0
  return 1
}

start_service() {
  local svc="$1"
  local bin
  bin="$(binary_of "$svc")"
  local port
  port="$(service_port "$(config_of "$svc")")"

  [ -x "$bin" ] || {
    err "missing $bin; run scripts/build-all.sh $PROFILE"
    return 1
  }
  if [ -n "$(running_pid "$svc")" ]; then
    log "$svc already running"
    return 0
  fi
  if port_busy "$port"; then
    err "port $port is busy and is not this sandbox's $svc"
    return 1
  fi

  (
    cd "$STACK_DIR/$svc" || exit 1
    exec setsid --fork "$bin" >> "$STACK_DIR/logs/$svc.log" 2>&1 < /dev/null
  ) &
  local attempt
  for attempt in $(seq 1 40); do
    [ -n "$(running_pid "$svc")" ] && break
    sleep 0.5
  done
  local pid
  pid="$(running_pid "$svc")"
  [ -n "$pid" ] || {
    err "$svc did not start; see $STACK_DIR/logs/$svc.log"
    return 1
  }
  printf '%s\n' "$pid" > "$STACK_DIR/pids/$svc.pid"
  log "$svc started pid=$pid port=$port"
}

up() {
  prepare || return 1

  local svc
  for svc in "${SERVICES[@]}"; do
    start_service "$svc" || return 1
  done

  for svc in "${SERVICES[@]}"; do
    wait_health "$svc" "$(service_port "$(config_of "$svc")")" || return 1
  done
  log "native stack up under $STACK_DIR"
}

stop_service() {
  local svc="$1"
  local bin
  bin="$(binary_of "$svc")"

  if pkill -f "^${bin}\$"; then
    local attempt
    for attempt in $(seq 1 20); do
      [ -z "$(running_pid "$svc")" ] && break
      sleep 0.5
    done
    if [ -n "$(running_pid "$svc")" ]; then
      err "$svc ignored SIGTERM and is still running"
      return 1
    fi
    log "$svc stopped"
  else
    log "$svc was not running"
  fi
  rm -f "$STACK_DIR/pids/$svc.pid"
}

hard_kill_service() {
  local svc="$1"
  local pids
  pids="$(running_pid "$svc")"

  if [ -z "$pids" ]; then
    log "$svc was not running"
  else
    local pid
    for pid in $pids; do
      kill -9 "$pid" 2>/dev/null || true
    done
    local attempt
    for attempt in $(seq 1 20); do
      [ -z "$(running_pid "$svc")" ] && break
      sleep 0.5
    done
    if [ -n "$(running_pid "$svc")" ]; then
      err "$svc survived SIGKILL"
      return 1
    fi
    log "$svc killed without a drain pid=$(printf '%s' "$pids" | tr '\n' ' ')"
  fi
  rm -f "$STACK_DIR/pids/$svc.pid"
}

freeze_service() {
  local svc="$1"
  local pids
  pids="$(running_pid "$svc")"

  [ -n "$pids" ] || { err "$svc is not running"; return 1; }
  local pid
  for pid in $pids; do
    kill -STOP "$pid" || return 1
  done
  log "$svc frozen pid=$(printf '%s' "$pids" | tr '\n' ' ')"
}

down() {
  local svc result=0
  for svc in "${SERVICES[@]}"; do
    stop_service "$svc" || result=1
  done
  pkill -f "^$ROOT/third_party/go2rtc/go2rtc" 2>/dev/null \
    && log "go2rtc stopped" || true
  return "$result"
}

status() {
  local svc
  for svc in "${SERVICES[@]}"; do
    local pid
    pid="$(running_pid "$svc")"
    if [ -n "$pid" ]; then
      printf '  %-14s RUNNING %s\n' "$svc" "$(printf '%s' "$pid" | tr '\n' ' ')"
    else
      printf '  %-14s stopped\n' "$svc"
    fi
  done
}

harness_env() {
  local sync_config auth_config camera_config
  sync_config="$(config_of sync)"
  auth_config="$(config_of auth)"
  camera_config="$(config_of camera)"
  [ -f "$sync_config" ] || sync_config="$ROOT/services/sync/config.toml"
  [ -f "$auth_config" ] || auth_config="$ROOT/services/auth/config.toml"
  [ -f "$camera_config" ] || camera_config="$ROOT/services/camera/config.toml"

  python3 "$ROOT/scripts/seed-golden.py" --stack-dir "$STACK_DIR" \
    --token-only >&2

  printf 'export ARGUS_TEST_BASE_URL=https://127.0.0.1:%s\n' \
    "$(service_port "$sync_config")"
  printf 'export ARGUS_TEST_AUTH_BASE_URL=https://127.0.0.1:%s\n' \
    "$(service_port "$auth_config")"
  printf 'export ARGUS_TEST_MEDIA_BASE_URL=https://127.0.0.1:%s\n' \
    "$(service_port "$camera_config")"
  printf 'export ARGUS_TEST_FIXTURES_DIR=%s/services/sync/tests/fixtures/sync\n' \
    "$ROOT"
  printf 'export ARGUS_TEST_REFRESH_TOKEN=%s\n' "$(cat "$(token_file)")"
}

require_service() {
  local svc="${1:-}"
  [ -n "$svc" ] || { usage; exit 2; }
  local known
  for known in "${SERVICES[@]}"; do
    [ "$known" = "$svc" ] && return 0
  done
  err "unknown service '$svc'"
  exit 2
}

case "${1:-}" in
  up) up ;;
  down) down ;;
  restart)
    require_service "${2:-}"
    stop_service "$2" && ensure_prepared "$2" || exit 1
    if settings_owner "$2"; then wire_settings_owners; fi
    start_service "$2"
    ;;
  kill) require_service "${2:-}"; stop_service "$2" ;;
  sigkill) require_service "${2:-}"; hard_kill_service "$2" ;;
  freeze) require_service "${2:-}"; freeze_service "$2" ;;
  status) status ;;
  logs) require_service "${2:-}"; tail -n "${3:-40}" "$STACK_DIR/logs/$2.log" ;;
  prepare) prepare ;;
  env) harness_env ;;
  *) usage; exit 2 ;;
esac
