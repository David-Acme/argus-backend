#!/usr/bin/env bash

set -euo pipefail
set -E
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

PROJECTS=(
  packages/lib/cert
  packages/lib/sqlite
  services/auth
  services/identity
  services/sync
  services/camera
  services/productivity
  services/notification
  services/guard
  services/settings
  services/tts
  services/stt
  services/vlm
  services/llm
  services/voice
  services/tunnel
)

PROFILE="dev"
NO_TESTS=0
INSTALL_ONLY=0
ONLY=""
JOBS=""

MEM_RESERVE_MB=6144
MEM_PER_JOB_MB=4096

usage() {
  echo "usage: $0 [dev|prod] [--jobs N] [--no-tests] [--install-only] [--only <project>]"
}

is_job_count() {
  case "$1" in ""|*[!0-9]*) return 1 ;; esac
  [ "$1" -ge 1 ]
}

mem_available_mb() {
  awk '/^MemAvailable:/ { printf "%d", $2 / 1024 }' /proc/meminfo || printf '0'
}

cpu_count() {
  nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n'
}

cgroup_headroom_mb() {
  local rel dir max current
  rel="$(awk -F: '$1 == "0" { print $3 }' /proc/self/cgroup 2>/dev/null)"
  [ -n "$rel" ] || return 1
  dir="/sys/fs/cgroup${rel%/}"
  [ -r "$dir/memory.max" ] || return 1
  max="$(cat "$dir/memory.max" 2>/dev/null)"
  case "$max" in ""|max|*[!0-9]*) return 1 ;; esac
  current="$(cat "$dir/memory.current" 2>/dev/null)"
  case "$current" in ""|*[!0-9]*) return 1 ;; esac
  [ "$max" -gt "$current" ] || return 1
  printf '%d' "$(( (max - current) / 1048576 ))"
}

derived_jobs() {
  local host_mb="$1" cgroup_mb="$2" cpus="$3" budget jobs
  budget=$(( host_mb - MEM_RESERVE_MB ))
  [ "$budget" -lt 0 ] && budget=0
  if [ -n "$cgroup_mb" ] && [ "$cgroup_mb" -lt "$budget" ]; then
    budget="$cgroup_mb"
  fi
  jobs=$(( budget / MEM_PER_JOB_MB ))
  [ "$jobs" -lt 1 ] && jobs=1
  [ "$jobs" -gt "$cpus" ] && jobs="$cpus"
  printf '%s' "$jobs"
}

while [ $# -gt 0 ]; do
  case "$1" in
    dev|prod)   PROFILE="$1" ;;
    --jobs)     [ $# -ge 2 ] || { usage; exit 1; }; JOBS="$2"; shift ;;
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

if [ -n "$JOBS" ]; then
  is_job_count "$JOBS" || { err "invalid --jobs value: $JOBS"; usage; exit 1; }
  log "build jobs: $JOBS (--jobs)"
elif [ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]; then
  is_job_count "$CMAKE_BUILD_PARALLEL_LEVEL" || {
    err "invalid CMAKE_BUILD_PARALLEL_LEVEL: $CMAKE_BUILD_PARALLEL_LEVEL"
    exit 1
  }
  JOBS="$CMAKE_BUILD_PARALLEL_LEVEL"
  log "build jobs: $JOBS (CMAKE_BUILD_PARALLEL_LEVEL)"
else
  MEM_AVAILABLE_MB="$(mem_available_mb)"
  MEM_CGROUP_MB="$(cgroup_headroom_mb || true)"
  CPUS="$(cpu_count)"
  JOBS="$(derived_jobs "$MEM_AVAILABLE_MB" "$MEM_CGROUP_MB" "$CPUS")"
  if [ -n "$MEM_CGROUP_MB" ]; then
    MEM_CGROUP_TEXT="${MEM_CGROUP_MB} MiB"
  else
    MEM_CGROUP_TEXT="unlimited"
  fi
  log "build jobs: $JOBS (memory-derived: MemAvailable ${MEM_AVAILABLE_MB} MiB - reserve ${MEM_RESERVE_MB} MiB, cgroup headroom ${MEM_CGROUP_TEXT}, over ${MEM_PER_JOB_MB} MiB per job, ${CPUS} cpus)"
fi

log "=== comments ==="
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  "$ROOT/scripts/check-comments.sh"
else
  log "comments: $ROOT is not a git work tree, so the file list is unknown; the gate runs where the repository is"
fi

log "=== dependencies (section 2.4) ==="
"$ROOT/scripts/check-deps.sh"

log "=== routes ==="
"$ROOT/scripts/check-routes.sh"

CONAN_OUT="$ROOT/build/$PROFILE"
GENERATORS="$CONAN_OUT/build/$BUILD_TYPE/generators"

log "=== conan install (root manifest, $PROFILE) ==="
conan install "$ROOT" --output-folder="$CONAN_OUT" -s "build_type=$BUILD_TYPE" --build=missing \
  -c "tools.build:jobs=$JOBS"

if [ "$INSTALL_ONLY" -eq 1 ]; then
  log "Dependencies installed for all selected projects (profile: $PROFILE)."
  exit 0
fi

CURRENT_PROJECT=""
trap 'err "project failed: $CURRENT_PROJECT"' ERR

for dir in "${PROJECTS[@]}"; do
  name="$(basename "$dir")"
  if [ -n "$ONLY" ] && [ "$name" != "$ONLY" ]; then continue; fi
  CURRENT_PROJECT="$name"
  extra_targets=()
  case "$dir" in
    services/identity)     extra_targets=(argus-migrate-identity) ;;
    services/camera)       extra_targets=(argus-migrate-camera argus-vulkan-probe) ;;
    services/productivity) extra_targets=(argus-migrate-productivity) ;;
    services/notification) extra_targets=(argus-migrate-notification) ;;
    services/sync)         extra_targets=(argus-migrate-sync) ;;
  esac
  log "=== $name ($PROFILE) ==="
  (
    cd "$ROOT/$dir"
    cmake -S . -B "build/$PROFILE" -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_TOOLCHAIN_FILE="$GENERATORS/conan_toolchain.cmake" \
      -DCMAKE_PREFIX_PATH="$GENERATORS" \
      -DCMAKE_CXX_STANDARD=20 \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build "build/$PROFILE" -j "$JOBS"
    if [ "${#extra_targets[@]}" -gt 0 ]; then
      cmake --build "build/$PROFILE" -j "$JOBS" --target "${extra_targets[@]}"
    fi
    if [ "$NO_TESTS" -eq 0 ]; then
      cd "build/$PROFILE"
      ctest --output-on-failure
    fi
  )
done

CURRENT_PROJECT=""
if [ "$NO_TESTS" -eq 0 ] && [ -z "$ONLY" ]; then
  log "=== rules 16 and 19 (clang-tidy) ==="
  "$ROOT/scripts/check-tidy.sh"
fi

if [ "$NO_TESTS" -eq 1 ]; then
  log "All selected projects built; tests skipped (profile: $PROFILE)."
else
  log "All selected projects built and tested (profile: $PROFILE)."
fi
