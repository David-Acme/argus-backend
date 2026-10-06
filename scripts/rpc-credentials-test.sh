#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

source "$ROOT/scripts/lib/common.sh"

fail() {
  printf 'rpc-credentials-test: %s\n' "$*" >&2
  exit 1
}

PAIRS="auth:identity:credential:identity
camera:identity:credential:identity
guard:identity:credential:identity
llm:identity:credential:identity
notification:identity:credential:identity
productivity:identity:credential:identity
sync:identity:credential:identity
voice:identity:credential:identity
camera:auth:credential:auth
guard:auth:credential:auth
identity:auth:credential:auth
notification:auth:credential:auth
productivity:auth:credential:auth
settings:auth:credential:auth
sync:auth:credential:auth
identity:sync:control_credential:sync
notification:sync:control_credential:sync
camera:modules:credential:settings
guard:modules:credential:settings
identity:modules:credential:settings
productivity:modules:credential:settings
notification:modules:credential:settings
llm:modules:credential:settings
llm:guard:credential:guard
sync:modules:credential:settings"

DATA_OWNERS="identity:rpc.callers:settings:7040
productivity:grpc:caller_settings:7037
sync:rpc.callers:settings:7041"

check_data_owners() {
  local deploy_dir="$1"
  local owner table key port credential served
  while IFS=: read -r owner table key port; do
    credential="$(toml_value "$deploy_dir/config.settings.toml" "owners.$owner" credential)"
    served="$(toml_value "$deploy_dir/config.$owner.toml" "$table" "$key")"
    [ "${#credential}" -eq 64 ] || fail "deploy: settings has no credential for the data owner $owner"
    [ "$credential" = "$served" ] || fail "deploy: settings and $owner disagree on [$table] $key"
    [ "$(toml_value "$deploy_dir/config.settings.toml" "owners.$owner" target)" = "argus-$owner:$port" ] ||
      fail "deploy: settings does not target the data owner $owner on $port"
    if grep -Fxq "$credential" "$TEST_TMP/seen-deploy"; then
      fail "deploy: the data owner $owner reuses another pair's credential"
    fi
  done <<<"$DATA_OWNERS"
}

check_pairs() {
  local mode="$1"
  local base="$2"
  local seen="$TEST_TMP/seen-$mode"
  local caller table key server client_value server_value
  : > "$seen"
  while IFS=: read -r caller table key server; do
    client_value="$(toml_value "$(fleet_caller_config "$mode" "$base" "$caller")" "$table" "$key")"
    server_value="$(toml_value "$(fleet_caller_config "$mode" "$base" "$server")" rpc.callers "$caller")"
    [ -n "$client_value" ] || fail "$mode: $caller has no [$table] $key"
    case "$client_value" in *CHANGE_ME*) fail "$mode: $caller [$table] $key is a placeholder" ;; esac
    [ "${#client_value}" -eq 64 ] || fail "$mode: $caller [$table] $key is not 32 random bytes"
    [ "$client_value" = "$server_value" ] ||
      fail "$mode: $caller [$table] $key differs from $server [rpc.callers] $caller"
    if grep -Fxq "$client_value" "$seen"; then
      fail "$mode: $caller -> $server reuses another pair's credential"
    fi
    printf '%s\n' "$client_value" >> "$seen"
  done <<<"$PAIRS"
}

snapshot() {
  cat "$@" | sha256sum | cut -d' ' -f1
}

deploy="$TEST_TMP/deploy"
mkdir -p "$deploy"
cp "$ROOT"/argus-deploy/config.*.toml.example "$deploy"/
[ -f "$ROOT/argus-deploy/livekit.yaml.example" ] && cp "$ROOT/argus-deploy/livekit.yaml.example" "$deploy"/
ensure_deploy_configs "$deploy" > "$TEST_TMP/deploy-first.log" 2>&1 || fail "ensure_deploy_configs failed"
check_pairs deploy "$deploy"
check_data_owners "$deploy"
for service in camera guard identity llm notification productivity sync; do
  [ "$(toml_value "$deploy/config.$service.toml" modules target)" = "argus-settings:7047" ] ||
    fail "deploy: $service does not read the enabled set from argus-settings:7047"
done
llm_tools="$(toml_value "$deploy/config.llm.toml" productivity credential)"
[ "${#llm_tools}" -eq 64 ] || fail "deploy: llm has no productivity credential"
[ "$llm_tools" = "$(toml_value "$deploy/config.productivity.toml" grpc caller_llm)" ] ||
  fail "deploy: llm and productivity disagree on the tools credential"
while IFS= read -r value; do
  if grep -Fq "$value" "$TEST_TMP/deploy-first.log"; then
    fail "a credential was printed to the console"
  fi
done < "$TEST_TMP/seen-deploy"
for template in config.identity config.auth config.sync; do
  if toml_key_exists "$deploy/$template.toml" identity rpc_secret ||
    toml_key_exists "$deploy/$template.toml" auth rpc_secret ||
    toml_key_exists "$deploy/$template.toml" sync control_secret; then
    fail "a fresh $template.toml carries a fleet-wide secret"
  fi
done

before="$(snapshot "$deploy"/config.*.toml)"
ensure_deploy_configs "$deploy" > /dev/null 2>&1 || fail "the second ensure_deploy_configs failed"
[ "$(snapshot "$deploy"/config.*.toml)" = "$before" ] || fail "a second provisioning changed a config"

upgrade="$TEST_TMP/upgrade"
mkdir -p "$upgrade"
cp "$deploy"/config.*.toml.example "$upgrade"/
cp "$deploy"/config.*.toml "$upgrade"/
legacy="$(openssl rand -hex 32)"
for service in auth camera guard identity llm notification productivity settings sync voice; do
  config="$upgrade/config.$service.toml"
  remove_toml_key identity credential "$config"
  remove_toml_key auth credential "$config"
  remove_toml_key sync control_credential "$config"
  for caller in auth camera guard identity llm notification productivity settings sync voice; do
    case "$service" in identity | auth | sync) remove_toml_key rpc.callers "$caller" "$config" ;; esac
  done
done
replace_toml_value identity rpc_secret "$legacy" "$upgrade/config.identity.toml"
replace_toml_value identity rpc_secret "$legacy" "$upgrade/config.camera.toml"
actions="$(toml_value "$upgrade/config.guard.toml" camera actions_credential)"
ensure_deploy_configs "$upgrade" > /dev/null 2>&1 || fail "upgrading an existing deploy failed"
check_pairs deploy "$upgrade"
[ "$(toml_value "$upgrade/config.identity.toml" identity rpc_secret)" = "$legacy" ] ||
  fail "the upgrade rotated the legacy fleet secret"
[ "$(toml_value "$upgrade/config.camera.toml" identity rpc_secret)" = "$legacy" ] ||
  fail "the upgrade rotated a caller's legacy fleet secret"
[ "$(toml_value "$upgrade/config.guard.toml" camera actions_credential)" = "$actions" ] ||
  fail "the upgrade rotated an existing caller credential"

native="$TEST_TMP/native"
for service in auth camera guard identity llm notification productivity settings sync voice; do
  mkdir -p "$native/services/$service"
  cp "$ROOT/services/$service/config.toml.example" "$native/services/$service/config.toml"
done
kept="$(openssl rand -hex 32)"
replace_toml_value rpc.callers voice "$kept" "$native/services/identity/config.toml"
ensure_fleet_callers native "$native"
check_pairs native "$native"
[ "$(toml_value "$native/services/voice/config.toml" identity credential)" = "$kept" ] ||
  fail "a server-side credential was not adopted by its caller"
before="$(snapshot "$native"/services/*/config.toml)"
ensure_fleet_callers native "$native"
[ "$(snapshot "$native"/services/*/config.toml)" = "$before" ] || fail "a second native pairing changed a config"

printf 'rpc-credentials-test: ok\n'
