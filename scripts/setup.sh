#!/usr/bin/env bash
#
# Argus backend - setup script (Linux).
#
# Provisions local dependencies and builds every standalone project:
#   1. Install system build dependencies (distro aware)
#   2. Install Conan (if missing) and configure the profile for C++20
#   3. Download the Supertonic 3 TTS model (~415 MB)
#   4. Download the LFM2.5-1.2B-Instruct QAD LLM (~696 MB)
#   5. Install, configure, build and test every standalone project
#   6. Create per-project local configs
#
# Usage:
#   ./scripts/setup.sh                     # default: dev
#   ./scripts/setup.sh dev                 # dev profile
#   ./scripts/setup.sh prod                # prod profile
#   ./scripts/setup.sh dev --no-build      # install deps only (no compile)
#   ./scripts/setup.sh camera              # camera artifacts only (detector + go2rtc)
#   SKIP_BUILD=1 ./scripts/setup.sh prod
#
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

PROFILE="dev"
SKIP_BUILD="${SKIP_BUILD:-0}"
CAMERA_ONLY=0
ARGS=()

for a in "$@"; do
  case "$a" in
    -h|--help)
      grep '^#' "$0" | sed 's/^#\{1,2\} //'; exit 0 ;;
    --no-build) SKIP_BUILD=1 ;;
    camera)     CAMERA_ONLY=1 ;;
    dev|prod)   PROFILE="$a" ;;
    *)          ARGS+=("$a") ;;
  esac
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug" ;;
  prod) BUILD_TYPE="Release" ;;
esac

log "Profile: $PROFILE  build_type: $BUILD_TYPE"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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

  # Local build tweaks for third-party submodules. The submodules point at
  # upstream repos (no push access), so these one-line patches are re-applied
  # on every setup run (idempotent) to keep the build reproducible.
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

setup_certs() {
  log "Setting up local PKI (instance CA + server certificate)..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local CERT_DIR="$ROOT/certs"

  need_cmd openssl
  mkdir -p "$CERT_DIR"

  if [ ! -f "$CERT_DIR/ca.pem" ]; then
    log "Generating instance CA (10 years) and server leaf (90 days)..."

    # Instance CA: EC P-256 key + self-signed cert (the per-instance identity).
    # 25 years validity, matching the long-lived root CAs Google ships in
    # Android. The CA never rotates; only the server leaf does.
    openssl ecparam -name prime256v1 -genkey -noout -out "$CERT_DIR/ca.key"
    openssl req -x509 -new -key "$CERT_DIR/ca.key" -sha256 -days 9125 \
      -subj "/CN=Argus Instance CA" -out "$CERT_DIR/ca.pem"

    # Server leaf key.
    openssl ecparam -name prime256v1 -genkey -noout -out "$CERT_DIR/server.key"

    # SANs: advertised hostname, configurable mdns.name, localhost/loopback.
    local MDNS_NAME=""
    local GW_CONFIG="$ROOT/services/argus-gateway/config.toml"
    if [ -f "$GW_CONFIG" ]; then
      MDNS_NAME="$(sed -n 's/^[[:space:]]*name *= *"\([^"]*\)".*/\1/p' "$GW_CONFIG" | head -1)"
    fi
    [ -z "$MDNS_NAME" ] && MDNS_NAME="Argus"
    local SAN="DNS:argus.local,DNS:localhost,IP:127.0.0.1,IP:::1"
    local HN="$(hostname 2>/dev/null)"
    [ -n "$HN" ] && SAN="$SAN,DNS:$HN"
    case "x$MDNS_NAME" in
      x[A-Za-z0-9_-]*) SAN="$SAN,DNS:${MDNS_NAME}" ;;
    esac

    openssl req -new -key "$CERT_DIR/server.key" \
      -subj "/CN=${MDNS_NAME}.local" -out "$CERT_DIR/server.csr"
    openssl x509 -req -in "$CERT_DIR/server.csr" \
      -CA "$CERT_DIR/ca.pem" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
      -sha256 -days 90 -extfile <(printf "subjectAltName=%s" "$SAN") \
      -out "$CERT_DIR/server.pem"
    rm -f "$CERT_DIR/server.csr"

    # Chain file: leaf + CA, so Drogon sends the full chain in the handshake.
    cat "$CERT_DIR/ca.pem" >> "$CERT_DIR/server.pem"

    chmod 600 "$CERT_DIR/ca.key" "$CERT_DIR/server.key"
  else
    log "PKI already present."
  fi

  # Pairing code = first 8 hex chars of the CA SHA-256 fingerprint. Both the
  # server (CertService) and the clients derive it; it gates POST /pairing.
  local FP
  FP="$(openssl x509 -in "$CERT_DIR/ca.pem" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g')"
  echo "${FP:0:8}" > "$CERT_DIR/pairing.code"
  chmod 600 "$CERT_DIR/pairing.code"

  log "CA fingerprint (SHA-256): $FP"
  log "Pairing code: ${FP:0:8}"
}

ensure_local_config() {
  need_cmd openssl
  local dir
  for dir in \
      services/argus-gateway \
      services/argus-camera \
      services/argus-productivity \
      services/argus-notification \
      services/argus-tts \
      services/argus-stt \
      services/argus-vlm \
      services/argus-llm \
      services/argus-voice \
      services/argus-tunnel \
      packages/argus-memory; do
    ensure_project_config "$ROOT/$dir" || exit 1
  done
  log "Per-project configs are ready."
}

main() {
  if [ "$CAMERA_ONLY" -eq 1 ]; then
    "$ROOT/services/argus-camera/scripts/provision.sh"
    exit 0
  fi

  ensure_local_config

  # Hardware detection installs the GPU/video/audio stack for this specific
  # host and writes scripts/.hw-profile. It asks for sudo only if something is
  # actually missing. install_system_deps stays as the minimal fallback.
  if [ -x "$(dirname "$0")/detect-hardware.sh" ]; then
    "$(dirname "$0")/detect-hardware.sh" ${ARGUS_ASSUME_YES:+-y} || \
      warn "Hardware detection failed; falling back to the base dependency set."
  fi
  install_system_deps
  ensure_conan
  configure_conan_profile
  need_cmd git
  need_cmd cmake
  setup_submodules
  setup_certs
  "$ROOT/services/argus-tts/scripts/provision.sh"
  "$ROOT/services/argus-llm/scripts/provision.sh"
  "$ROOT/services/argus-vlm/scripts/provision.sh"
  "$ROOT/services/argus-stt/scripts/provision.sh"
  "$ROOT/packages/argus-identity/scripts/provision.sh"
  "$ROOT/services/argus-voice/scripts/provision.sh"
  "$ROOT/packages/argus-memory/scripts/provision.sh"
  "$ROOT/services/argus-camera/scripts/provision.sh"
  build_project
  log "All setup tasks completed (profile: $PROFILE)."
}

main "$@"
