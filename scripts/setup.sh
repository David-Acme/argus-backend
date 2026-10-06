#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/pki.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/docker.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/privacy.sh"

PROFILE="dev"
SKIP_BUILD="${SKIP_BUILD:-0}"
CAMERA_ONLY=0
WITH_DOCKER=1
ASSUME_YES="${ARGUS_ASSUME_YES:+1}"
ARGS=()
PRIVACY_ACTION="require"

usage() {
  cat <<'USAGE'
Argus backend - setup script (Linux).

Shows the Argus privacy notice and terms first and configures nothing until
they are accepted. Then provisions local dependencies and builds every
standalone project:
  1. Install system build dependencies (distro aware)
  2. Install Conan (if missing) and configure the profile for C++20
  3. Download Supertonic 3 (~415 MB) and export the selected Kyutai Pocket TTS models (pinned)
  4. Download the LFM2.5-1.2B-Instruct QAD LLM (~696 MB)
  5. Install, configure, build and test every standalone project
  6. Create per-project local configs

Usage:
  ./scripts/setup.sh                     default: dev
  ./scripts/setup.sh dev                 dev profile
  ./scripts/setup.sh prod                prod profile
  ./scripts/setup.sh dev --no-build      install deps only (no compile)
  ./scripts/setup.sh dev --no-docker     do not install/validate Docker
  ./scripts/setup.sh dev -y              non-interactive package installs
  ./scripts/setup.sh camera              camera artifacts only (detector + go2rtc)
  SKIP_BUILD=1 ./scripts/setup.sh prod

Privacy notice (required before anything is configured):
  --accept-privacy-notice      accept the notice in a non-interactive run
                               (also ARGUS_ACCEPT_PRIVACY_NOTICE=1); without it
                               a non-interactive run stops; -y never accepts it
  --accept-visitor-notice      also acknowledge recurring-visitor recognition
                               (it stays off until enabled in the app)
  --privacy-lang=es|en         notice language (default: from LANG, else es)
  --jurisdiction=pe            country rules quoted in the notice
                               (scripts/privacy/jurisdictions.tsv)
  --show-privacy-notice        print the notice and exit
  --withdraw-privacy-consent   remove the recorded acceptance and exit
  The acceptance (version, time, user@host) is stored in
  ${ARGUS_DATA_DIR:-argus-deploy/data}/privacy/host-consent.json (0600).
USAGE
}

for a in "$@"; do
  case "$a" in
    -h|--help)
      usage; exit 0 ;;
    --no-build) SKIP_BUILD=1 ;;
    --no-docker) WITH_DOCKER=0 ;;
    -y|--yes)   ASSUME_YES=1 ;;
    --accept-privacy-notice) PRIVACY_ACCEPT=1 ;;
    --accept-visitor-notice) PRIVACY_VISITOR_ACCEPT=1 ;;
    --privacy-lang=*) PRIVACY_LANG="${a#*=}" ;;
    --jurisdiction=*) PRIVACY_JURISDICTION="${a#*=}" ;;
    --show-privacy-notice) PRIVACY_ACTION="show" ;;
    --withdraw-privacy-consent) PRIVACY_ACTION="withdraw" ;;
    camera)     CAMERA_ONLY=1 ;;
    dev|prod)   PROFILE="$a" ;;
    *)          ARGS+=("$a") ;;
  esac
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug" ;;
  prod) BUILD_TYPE="Release" ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRIVACY_DATA_DIR="${ARGUS_DATA_DIR:-$ROOT/argus-deploy/data}"

case "$PRIVACY_ACTION" in
  show) privacy_notice_text; exit $? ;;
  withdraw) privacy_withdraw_consent "$PRIVACY_DATA_DIR"; exit 0 ;;
esac
need_cmd openssl
privacy_require_consent "$PRIVACY_DATA_DIR" "scripts/setup.sh"

log "Profile: $PROFILE  build_type: $BUILD_TYPE"

install_system_deps() {
  log "Detecting distribution and installing build dependencies..."

  local missing=0
  for c in git cmake ninja g++ python3 openssl; do
    command -v "$c" >/dev/null 2>&1 || missing=1
  done
  if [ "$missing" -eq 0 ]; then
    log "Build dependencies already installed, skipping system package install."
    return
  fi

  local pkg_mgr=""
  if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "${ID:-},${ID_LIKE:-}" in
      *arch*|*manjaro*) pkg_mgr="pacman" ;;
      *debian*|*ubuntu*) pkg_mgr="apt" ;;
      *fedora*|*rhel*) pkg_mgr="dnf" ;;
      *alpine*) pkg_mgr="apk" ;;
      *opensuse*) pkg_mgr="zypper" ;;
    esac
  fi

  case "$pkg_mgr" in
    pacman)
      sudo_if_needed pacman -Syu --needed --noconfirm git cmake ninja gcc python python-pip openssl base-devel \
        vulkan-headers spirv-headers shaderc vulkan-icd-loader ;;
    apt)
      sudo_if_needed apt-get update -y
      sudo_if_needed apt-get install -y --no-install-recommends git cmake ninja-build g++ python3 python3-pip pkg-config openssl ca-certificates \
        libvulkan-dev spirv-headers glslc ;;
    dnf)
      sudo_if_needed dnf install -y git cmake ninja-build gcc-c++ python3 python3-pip openssl \
        vulkan-headers spirv-headers glslc vulkan-loader-devel ;;
    apk)
      sudo_if_needed apk add --no-cache git cmake ninja g++ python3 py3-pip openssl build-base linux-headers ;;
    zypper)
      sudo_if_needed zypper install -y git cmake ninja gcc-c++ python3 python3-pip openssl ;;
    *)
      warn "Unknown distribution. Ensure git cmake ninja g++ python3 pip openssl are installed."
      warn "For GPU offload also install: Vulkan headers/loader, glslc, SPIRV-Headers." ;;
  esac
}

ensure_conan() {
  if command -v conan >/dev/null 2>&1; then
    log "Conan already installed: $(conan --version)"
    return
  fi
  log "Conan not found, installing via pip..."
  local PIP=""
  command -v pip3 >/dev/null 2>&1 && PIP=pip3
  command -v pip  >/dev/null 2>&1 && PIP=pip
  [ -z "$PIP" ] && PIP="python3 -m pip"
  $PIP install --user conan 2>/dev/null || sudo_if_needed $PIP install conan 2>/dev/null || $PIP install --user --break-system-packages conan

  local pybin
  pybin="$(python3 -m site --user-base 2>/dev/null)/bin"
  case ":$PATH:" in
    *":$pybin:"*) ;;
    *) export PATH="$pybin:$PATH" ;;
  esac
}

configure_conan_profile() {
  log "Configuring Conan profile (default)..."
  if [ ! -f "$HOME/.conan2/profiles/default" ]; then
    conan profile detect
  fi
  local profile="$HOME/.conan2/profiles/default"
  if grep -q '^compiler.cppstd=' "$profile"; then
    sed -i 's/^compiler.cppstd=.*/compiler.cppstd=gnu20/' "$profile"
  else
    sed -i '/^compiler.version=/a compiler.cppstd=gnu20' "$profile"
  fi
  log "Conan profile ready: $profile"
}

setup_submodules() {
  log "Initialising git submodules (third_party/*)..."
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    warn "Not a git worktree; skipping submodule init."
    return
  fi
  git submodule update --init --recursive 2>&1 | sed 's/^/  /' || \
    warn "Top-level submodule init failed (network?)."

  for d in third_party/*/; do
    if [ -d "${d}.git" ] || git -C "$d" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      log "Initialising nested submodules in $d"
      git -C "$d" submodule update --init --recursive 2>&1 | sed 's/^/  /' || \
        warn "Nested submodule init failed in $d (network?)."
    fi
  done

  local F

  F=third_party/ncnn/CMakeLists.txt
  if [ -f "$F" ]; then
    sed -i 's/^\( *\)option(NCNN_BUILD_TOOLS "build tools" OFF)/\1option(NCNN_BUILD_TOOLS "build tools" ON)/' "$F"
    sed -i 's/^\( *\)set(NCNN_BUILD_TOOLS OFF)/\1#set(NCNN_BUILD_TOOLS OFF)/' "$F"
  fi

  F=third_party/sherpa-onnx/cmake/json.cmake
  if [ -f "$F" ]; then
    sed -i 's|^  add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)|  # disabled: nlohmann_json provided by Conan|' "$F"
  fi
}

build_project() {
  if [ "$SKIP_BUILD" -eq 1 ]; then
    "$ROOT/scripts/build-all.sh" "$PROFILE" --install-only
    log "Standalone dependencies installed; build skipped."
    return
  fi

  "$ROOT/scripts/build-all.sh" "$PROFILE"
  log "Standalone projects built and tested."
}

ensure_local_config() {
  need_cmd openssl
  local configs=() dir
  for dir in \
      services/auth \
      services/identity \
      services/sync \
      services/camera \
      services/guard \
      services/settings \
      services/productivity \
      services/notification \
      services/tts \
      services/stt \
      services/vlm \
      services/llm \
      services/voice \
      services/tunnel; do
    ensure_project_config "$ROOT/$dir" || exit 1
    configs+=("$ROOT/$dir/config.toml")
  done
  ensure_shared_configs "${configs[@]}"
  propagate_config_value "$ROOT/services/auth/config.toml" device identity_mode \
    "${configs[@]}"
  fill_config_pair "$ROOT/services/sync/config.toml" notifications credential \
    "$ROOT/services/notification/config.toml" grpc caller_sync 32
  fill_config_pair "$ROOT/services/sync/config.toml" productivity credential \
    "$ROOT/services/productivity/config.toml" grpc caller_sync 32
  fill_config_pair "$ROOT/services/sync/config.toml" voice credential \
    "$ROOT/services/voice/config.toml" grpc caller_sync 32
  fill_config_pair "$ROOT/services/sync/config.toml" camera credential \
    "$ROOT/services/camera/config.toml" grpc caller_sync 32
  fill_config_pair "$ROOT/services/llm/config.toml" camera credential \
    "$ROOT/services/camera/config.toml" grpc caller_llm 32
  fill_config_pair "$ROOT/services/llm/config.toml" productivity credential \
    "$ROOT/services/productivity/config.toml" grpc caller_llm 32
  fill_config_pair "$ROOT/services/guard/config.toml" camera actions_credential \
    "$ROOT/services/camera/config.toml" grpc caller_guard 32
  fill_config_pair "$ROOT/services/guard/config.toml" notifications credential \
    "$ROOT/services/notification/config.toml" grpc caller_guard 32
  fill_config_pair "$ROOT/services/productivity/config.toml" notifications credential \
    "$ROOT/services/notification/config.toml" grpc caller_productivity 32
  fill_config_pair "$ROOT/services/llm/config.toml" notifications credential \
    "$ROOT/services/notification/config.toml" grpc caller_llm 32
  fill_config_pair "$ROOT/services/voice/config.toml" llm grpc_credential \
    "$ROOT/services/llm/config.toml" rpc.callers voice 32
  fill_config_pair "$ROOT/services/voice/config.toml" notification credential \
    "$ROOT/services/notification/config.toml" grpc caller_voice 32
  fill_config_pair "$ROOT/services/notification/config.toml" voice credential \
    "$ROOT/services/voice/config.toml" grpc caller_notification 32
  fill_config_pair "$ROOT/services/sync/config.toml" guard presence_credential \
    "$ROOT/services/guard/config.toml" rpc.callers sync 32
  fill_config_pair "$ROOT/services/notification/config.toml" guard presence_credential \
    "$ROOT/services/guard/config.toml" rpc.callers notification 32
  ensure_fleet_callers native "$ROOT"
  ensure_settings_owners "$ROOT/services/settings/config.toml" native "$ROOT"
  ensure_livekit_key_pair "$ROOT/services/sync/config.toml"
  remove_toml_key tunnel max_reconnects "$ROOT/services/tunnel/config.toml"
  ensure_tunnel_secret "$ROOT/services/tunnel/config.toml"
  log "Per-project configs are ready."
}

sync_storage_credentials() {
  local data_dir="${ARGUS_DATA_DIR:-$ROOT/argus-deploy/data}"
  local secrets="$data_dir/rustfs/secrets"
  [ -r "$secrets/argus_s3_access_key" ] || return 0
  [ -r "$secrets/argus_s3_secret_key" ] || return 0
  [ -r "$secrets/rustfs-bucket" ] || return 0

  local bucket access secret
  bucket="$(tr -d '\r\n' < "$secrets/rustfs-bucket")"
  access="$(tr -d '\r\n' < "$secrets/argus_s3_access_key")"
  secret="$(tr -d '\r\n' < "$secrets/argus_s3_secret_key")"
  if [ -z "$bucket" ] || [ -z "$access" ] || [ -z "$secret" ]; then
    warn "RustFS secrets are incomplete; storage configs left untouched."
    return 0
  fi

  local config synced=0
  for config in "$ROOT"/argus-deploy/config.*.toml \
                "$ROOT"/argus-deploy/data/native/*/config.toml \
                "$ROOT"/services/*/config.toml; do
    [ -f "$config" ] || continue
    toml_key_exists "$config" "storage.s3" "bucket" || continue
    replace_toml_value storage.s3 bucket "$bucket" "$config"
    replace_toml_value storage.s3 access_key "$access" "$config"
    replace_toml_value storage.s3 secret_key "$secret" "$config"
    synced=$((synced + 1))
  done
  log "Object storage credentials synced into $synced config(s) from ${secrets#"$ROOT"/}."
}

provision_tts() {
  if [ "$PROFILE" = dev ] && [ -z "${ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES:-}" ]; then
    export ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1
    warn "Development setup: Kyutai's Pocket voice jean is licensed for non-commercial use only (CC BY-NC 4.0) and is installed for internal testing. ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=0 skips it; prod and provision-host.sh never install it unless asked."
  fi
  if [ "${ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES:-0}" = 1 ]; then
    replace_toml_value tts pocket_noncommercial_voices true "$ROOT/services/tts/config.toml" literal
  fi
  ARGUS_TTS_CONFIG="$ROOT/services/tts/config.toml" "$ROOT/services/tts/scripts/provision.sh"
}

main() {
  if [ "$CAMERA_ONLY" -eq 1 ]; then
    "$ROOT/services/camera/scripts/provision.sh"
    exit 0
  fi

  ensure_local_config
  sync_storage_credentials

  if [ -x "$(dirname "$0")/detect-hardware.sh" ]; then
    "$(dirname "$0")/detect-hardware.sh" ${ASSUME_YES:+-y} || \
      warn "Hardware detection failed; falling back to the base dependency set."
  fi
  install_system_deps
  if [ "$WITH_DOCKER" -eq 1 ]; then
    ensure_docker "$ASSUME_YES" || \
      warn "Docker not ready; run ./scripts/provision-host.sh later."
  fi
  ensure_conan
  configure_conan_profile
  need_cmd git
  need_cmd cmake
  setup_submodules
  ensure_instance_certs "$ROOT" "$ROOT/certs" "$ROOT/services/identity/config.toml"
  provision_tts
  "$ROOT/services/llm/scripts/provision.sh"
  "$ROOT/services/vlm/scripts/provision.sh"
  "$ROOT/services/stt/scripts/provision.sh"
  "$ROOT/services/identity/scripts/provision.sh"
  "$ROOT/services/voice/scripts/provision.sh"
  "$ROOT/services/camera/scripts/provision.sh"
  build_project
  log "All setup tasks completed (profile: $PROFILE)."
  log "Deployment host prep: ./scripts/provision-host.sh [--start]"
}

main "$@"
