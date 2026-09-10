#!/usr/bin/env bash
# Build and test every standalone project with its own Conan/CMake presets.

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
ONLY=""

usage() { echo "usage: $0 [dev|prod] [--no-tests] [--only <project>]"; }

while [ $# -gt 0 ]; do
  case "$1" in
    dev|prod)   PROFILE="$1" ;;
    --no-tests) NO_TESTS=1 ;;
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
need_cmd cmake
need_cmd ctest

CURRENT_PROJECT=""
trap 'err "project failed: $CURRENT_PROJECT"' ERR

for dir in "${PROJECTS[@]}"; do
  name="$(basename "$dir")"
  if [ -n "$ONLY" ] && [ "$name" != "$ONLY" ]; then continue; fi
  CURRENT_PROJECT="$name"
  log "=== $name ($PROFILE) ==="
  (
    cd "$ROOT/$dir"
    conan install . --output-folder="build/$PROFILE" -s "build_type=$BUILD_TYPE" --build=missing
    cmake --preset "$PROFILE"
    cmake --build --preset "$PROFILE" -j 8
    if [ "$NO_TESTS" -eq 0 ]; then
      cd "build/$PROFILE"
      ctest --output-on-failure
    fi
  )
done

CURRENT_PROJECT=""
log "All projects built and tested (profile: $PROFILE)."
