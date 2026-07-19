#!/usr/bin/env bash
#
# Argus backend - Development / Production environment setup (Linux).
#
# Automates the build with CMake presets:
#   1. Install system build dependencies (distro aware)
#   2. Install Conan (if missing) and configure the profile for C++20
#   3. Install Conan dependencies into build/<profile>
#   4. Configure and build with the matching CMake preset
#
# Usage:
#   ./scripts/setup-dev.sh                 # dev profile, full build
#   ./scripts/setup-dev.sh --profile prod  # prod profile, full build
#   ./scripts/setup-dev.sh --no-build      # install deps only (no compile)
#   SKIP_BUILD=1 ./scripts/setup-dev.sh
#
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

sudo_if_needed() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    command -v sudo >/dev/null 2>&1 && sudo "$@" || "$@"
  fi
}

PROFILE="dev"
SKIP_BUILD=0
PREV_ARG=""
for a in "$@"; do
  case "$a" in
    dev|prod)
      if [ "${PREV_ARG:-}" = "--profile" ]; then PROFILE="$a"; fi
      ;;
    --no-build) SKIP_BUILD=1 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^#\{1,2\} //'; exit 0 ;;
    *)
      if [ "${PREV_ARG:-}" = "--profile" ]; then PROFILE="$a"; fi
      ;;
  esac
  PREV_ARG="$a"
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug";   OUTPUT_FOLDER="build/dev";  CMAKE_PRESET="dev" ;;
  prod) BUILD_TYPE="Release"; OUTPUT_FOLDER="build/prod"; CMAKE_PRESET="prod" ;;
  *) err "unknown profile: $PROFILE (expected dev|prod)"; exit 1 ;;
esac

log "Profile: $PROFILE  build_type: $BUILD_TYPE  output: $OUTPUT_FOLDER  preset: $CMAKE_PRESET"

install_system_deps() {
  log "Detecting distribution and installing build dependencies..."

  local missing=0
  for c in git cmake ninja g++ python3; do
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
      sudo_if_needed pacman -Syu --needed --noconfirm git cmake ninja gcc python python-pip base-devel ;;
    apt)
      sudo_if_needed apt-get update -y
      sudo_if_needed apt-get install -y --no-install-recommends git cmake ninja-build g++ python3 python3-pip pkg-config ca-certificates ;;
    dnf)
      sudo_if_needed dnf install -y git cmake ninja-build gcc-c++ python3 python3-pip ;;
    apk)
      sudo_if_needed apk add --no-cache git cmake ninja g++ python3 py3-pip build-base linux-headers ;;
    zypper)
      sudo_if_needed zypper install -y git cmake ninja gcc-c++ python3 python3-pip ;;
    *)
      warn "Unknown distribution. Ensure git cmake ninja g++ python3 pip are installed." ;;
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

build_project() {
  if [ "$SKIP_BUILD" -eq 1 ]; then
    log "Installing Conan dependencies into $OUTPUT_FOLDER ($BUILD_TYPE)..."
    conan install . --output-folder="$OUTPUT_FOLDER" -s "build_type=$BUILD_TYPE" --build=missing -o drogon/*:with_sqlite=True
    rm -f CMakeUserPresets.json
    log "Skipping build (--no-build / SKIP_BUILD). Done."
    return
  fi

  log "Installing Conan dependencies into $OUTPUT_FOLDER ($BUILD_TYPE)..."
  conan install . --output-folder="$OUTPUT_FOLDER" -s "build_type=$BUILD_TYPE" --build=missing -o drogon/*:with_sqlite=True
  rm -f CMakeUserPresets.json

  log "Configuring with CMake preset '$CMAKE_PRESET'..."
  cmake --preset "$CMAKE_PRESET"

  log "Building with CMake preset '$CMAKE_PRESET'..."
  cmake --build --preset "$CMAKE_PRESET"

  log "Build complete. Run the server from: $OUTPUT_FOLDER/argus-backend"
}

main() {
  install_system_deps
  ensure_conan
  configure_conan_profile
  need_cmd git
  need_cmd cmake
  build_project
  log "All done (profile: $PROFILE). Happy hacking!"
}

main "$@"
