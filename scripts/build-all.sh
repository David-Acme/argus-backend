#!/usr/bin/env bash
# Build and test every standalone project with its own Conan/CMake presets,
# including the on-demand owner CLI tools.

set -euo pipefail
set -E
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

PROJECTS=(
  packages/argus-common
  packages/argus-contracts
  packages/argus-cert
  packages/argus-socket
  packages/argus-sqlite
  packages/argus-identity
  packages/argus-sync
  packages/argus-memory
  packages/argus-intent
  services/argus-gateway
  services/argus-camera
  services/argus-productivity
  services/argus-notification
  services/argus-tts
  services/argus-stt
  services/argus-vlm
  services/argus-llm
  services/argus-voice
  services/argus-tunnel
)

PROFILE="dev"
NO_TESTS=0
INSTALL_ONLY=0
ONLY=""

usage() {
  echo "usage: $0 [dev|prod] [--no-tests] [--install-only] [--only <project>]"
}

while [ $# -gt 0 ]; do
  case "$1" in
    dev|prod)   PROFILE="$1" ;;
    --no-tests) NO_TESTS=1 ;;
    --install-only) INSTALL_ONLY=1 ;;
    --only)     [ $# -ge 2 ] || { usage; exit 1; }; ONLY="$2"; shift ;;
    -h|--help)  usage; exit 0 ;;
    *)          err "unknown argument: $1"; usage; exit 1 ;;
  esac
  shift
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug" ;;
  prod) BUILD_TYPE="Release" ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

need_cmd conan
if [ "$INSTALL_ONLY" -eq 0 ]; then
  need_cmd cmake
  if [ "$NO_TESTS" -eq 0 ]; then
    need_cmd ctest
  fi
fi

if [ -n "$ONLY" ]; then
  project_found=0
  for dir in "${PROJECTS[@]}"; do
    if [ "$(basename "$dir")" = "$ONLY" ]; then
      project_found=1
      break
    fi
  done
  if [ "$project_found" -eq 0 ]; then
    err "unknown project for --only: $ONLY"
    exit 1
  fi
fi

CURRENT_PROJECT=""
trap 'err "project failed: $CURRENT_PROJECT"' ERR

for dir in "${PROJECTS[@]}"; do
  name="$(basename "$dir")"
  if [ -n "$ONLY" ] && [ "$name" != "$ONLY" ]; then continue; fi
  CURRENT_PROJECT="$name"
  extra_targets=()
  case "$dir" in
    packages/argus-identity)     extra_targets=(argus-migrate-identity) ;;
    services/argus-camera)       extra_targets=(argus-migrate-camera argus-vulkan-probe) ;;
    services/argus-productivity) extra_targets=(argus-migrate-productivity) ;;
    services/argus-notification) extra_targets=(argus-migrate-notification) ;;
  esac
  log "=== $name ($PROFILE) ==="
  (
    cd "$ROOT/$dir"
    conan install . --output-folder="build/$PROFILE" -s "build_type=$BUILD_TYPE" --build=missing
    if [ "$INSTALL_ONLY" -eq 1 ]; then
      exit 0
    fi
    cmake --preset "$PROFILE"
    cmake --build --preset "$PROFILE" -j 8
    if [ "${#extra_targets[@]}" -gt 0 ]; then
      cmake --build --preset "$PROFILE" -j 8 --target "${extra_targets[@]}"
    fi
    if [ "$NO_TESTS" -eq 0 ]; then
      cd "build/$PROFILE"
      ctest --output-on-failure
    fi
  )
done

CURRENT_PROJECT=""
if [ "$INSTALL_ONLY" -eq 1 ]; then
  log "Dependencies installed for all selected projects (profile: $PROFILE)."
elif [ "$NO_TESTS" -eq 1 ]; then
  log "All selected projects built; tests skipped (profile: $PROFILE)."
else
  log "All selected projects built and tested (profile: $PROFILE)."
fi
