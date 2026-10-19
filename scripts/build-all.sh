#!/usr/bin/env bash

set -euo pipefail
set -E
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

PROJECTS=(
  services/llm
  services/camera
  services/identity
  services/voice
  services/guard
  services/sync
  services/notification
  services/productivity
  services/auth
  services/settings
  services/tts
  services/stt
  services/vlm
  services/tunnel
  packages
)

PROFILE="dev"
NO_TESTS=0
INSTALL_ONLY=0
ONLY=""
JOBS=""

MEM_RESERVE_MB="${ARGUS_MEM_RESERVE_MB:-1024}"
MEM_PER_JOB_MB="${ARGUS_MEM_PER_JOB_MB:-1536}"
LINK_BUDGET_MB=3072

usage() {
  echo "usage: $0 [dev|prod|ci] [--jobs N] [--no-tests] [--install-only] [--only <project>[,<project>...]]"
  echo "  dev = Debug, prod = Release, ci = Debug without debug info (-O0 -g0), for the hosted CI runners"
}

is_job_count() {
  case "$1" in ""|*[!0-9]*) return 1 ;; esac
  [ "$1" -ge 1 ]
}

mem_available_mb() {
  if [ -r /proc/meminfo ]; then
    awk '/^MemAvailable:/ { printf "%d", $2 / 1024 }' /proc/meminfo || printf '0'
  elif command -v vm_stat >/dev/null 2>&1; then
    vm_stat | awk '
      /page size of/ { size = $8 }
      /^Pages (free|inactive|speculative|purgeable):/ { gsub(/\./, "", $NF); pages += $NF }
      END { printf "%d", pages * (size ? size : 16384) / 1048576 }'
  else
    printf '0'
  fi
}

cpu_count() {
  nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || printf '1\n'
}

jobserver_capable() {
  local version major minor
  [ "${BASH_VERSINFO[0]}" -gt 5 ] || { [ "${BASH_VERSINFO[0]}" -eq 5 ] && [ "${BASH_VERSINFO[1]}" -ge 1 ]; } || return 1
  command -v mkfifo >/dev/null 2>&1 || return 1
  version="$(ninja --version 2>/dev/null)" || return 1
  major="${version%%.*}"
  minor="${version#*.}"
  minor="${minor%%.*}"
  case "$major$minor" in ""|*[!0-9]*) return 1 ;; esac
  [ "$major" -gt 1 ] || { [ "$major" -eq 1 ] && [ "$minor" -ge 13 ]; }
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
    dev|prod|ci) PROFILE="$1" ;;
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
  ci)   BUILD_TYPE="Debug" ;;
esac

CONAN_PROFILE_ARGS=()
CMAKE_PROFILE_ARGS=()
if [ "$PROFILE" = "ci" ]; then
  if [ -z "${CONAN_HOME+x}" ]; then
    export CONAN_HOME="${CONAN_HOME_CI:-$HOME/.conan2-ci}"
  fi
  CONAN_PROFILE_ARGS=(
    -c 'tools.cmake.cmaketoolchain:extra_variables={"CMAKE_C_FLAGS_DEBUG": "-O0 -g0", "CMAKE_CXX_FLAGS_DEBUG": "-O0 -g0"}'
    -c 'tools.build:cflags=["-g0"]'
    -c 'tools.build:cxxflags=["-g0"]'
  )
  CMAKE_PROFILE_ARGS=(-DCMAKE_C_FLAGS_DEBUG=-O0\ -g0 -DCMAKE_CXX_FLAGS_DEBUG=-O0\ -g0)
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

need_cmd conan
if [ "$INSTALL_ONLY" -eq 0 ]; then
  need_cmd cmake
  if [ "$NO_TESTS" -eq 0 ]; then
    need_cmd ctest
  fi
fi

selected_by_only() {
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

if [ -n "$ONLY" ]; then
  IFS=',' read -r -a only_names <<< "$ONLY"
  for wanted in "${only_names[@]}"; do
    project_found=0
    for dir in "${PROJECTS[@]}"; do
      if [ "$(basename "$dir")" = "$wanted" ]; then
        project_found=1
        break
      fi
    done
    if [ "$project_found" -eq 0 ]; then
      err "unknown project for --only: $wanted"
      exit 1
    fi
  done
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

log "=== vendored patches ==="
if compgen -G "$ROOT/third_party/*/argus-patches" >/dev/null; then
  "$ROOT/scripts/check-vendored.sh"
else
  log "vendored: $ROOT holds no third_party/*/argus-patches, so no patch is pinned here; the gate runs where the vendored trees are"
fi

CONAN_OUT="$ROOT/build/$PROFILE"
GENERATORS="$CONAN_OUT/build/$BUILD_TYPE/generators"

if [ "$PROFILE" = "ci" ] && [ ! -f "$CONAN_HOME/profiles/default" ]; then
  log "=== conan profile detect (ci Conan home $CONAN_HOME) ==="
  conan profile detect
fi

log "=== conan install (root manifest, $PROFILE) ==="
DEPENDENCY_BUILD_TYPE="$BUILD_TYPE"
if [ "$PROFILE" = "dev" ]; then
  DEPENDENCY_BUILD_TYPE="Release"
fi
conan install "$ROOT" --output-folder="$CONAN_OUT" -s "build_type=$DEPENDENCY_BUILD_TYPE" \
  -s "&:build_type=$BUILD_TYPE" --build=missing \
  -c "tools.build:jobs=$JOBS" "${CONAN_PROFILE_ARGS[@]}"

if [ "$INSTALL_ONLY" -eq 1 ]; then
  log "Dependencies installed for all selected projects (profile: $PROFILE)."
  exit 0
fi

SELECTED=()
for dir in "${PROJECTS[@]}"; do
  name="$(basename "$dir")"
  if [ -n "$ONLY" ] && ! selected_by_only "$name"; then continue; fi
  SELECTED+=("$dir")
done

PARALLEL_PROJECTS="${ARGUS_PARALLEL_PROJECTS:-4}"
is_job_count "$PARALLEL_PROJECTS" || { err "invalid ARGUS_PARALLEL_PROJECTS: $PARALLEL_PROJECTS"; exit 1; }
[ "$PARALLEL_PROJECTS" -gt "${#SELECTED[@]}" ] && PARALLEL_PROJECTS="${#SELECTED[@]}"
[ "$PARALLEL_PROJECTS" -gt "$JOBS" ] && PARALLEL_PROJECTS="$JOBS"
if [ "$PARALLEL_PROJECTS" -gt 1 ] && ! jobserver_capable; then
  log "projects: one at a time (a shared job pool needs bash >= 5.1, ninja >= 1.13 and mkfifo)"
  PARALLEL_PROJECTS=1
fi

if [ "$PARALLEL_PROJECTS" -gt 1 ]; then
  LINK_MEMORY_MB=$(( $(mem_available_mb) - MEM_RESERVE_MB ))
  LINK_POOLS=$(( LINK_MEMORY_MB / LINK_BUDGET_MB / PARALLEL_PROJECTS ))
  [ "$LINK_POOLS" -lt 1 ] && LINK_POOLS=1
  export ARGUS_LINK_POOLS="${ARGUS_LINK_POOLS:-$LINK_POOLS}"
  log "projects: $PARALLEL_PROJECTS at a time over one pool of $JOBS jobs, link pool $ARGUS_LINK_POOLS per project"
fi

extra_targets_for() {
  case "$1" in
    services/identity)     printf '%s\n' argus-migrate-identity ;;
    services/camera)       printf '%s\n' argus-migrate-camera argus-vulkan-probe ;;
    services/productivity) printf '%s\n' argus-migrate-productivity ;;
    services/notification) printf '%s\n' argus-migrate-notification ;;
    services/sync)         printf '%s\n' argus-migrate-sync ;;
  esac
}

configure_project() {
  local dir="$1"
  (
    cd "$ROOT/$dir"
    cmake -S . -B "build/$PROFILE" -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_TOOLCHAIN_FILE="$GENERATORS/conan_toolchain.cmake" \
      -DCMAKE_PREFIX_PATH="$GENERATORS" \
      -DCMAKE_CXX_STANDARD=20 \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DARGUS_SYSTEM_PROTOBUF="${ARGUS_SYSTEM_PROTOBUF:-OFF}" \
      "${CMAKE_PROFILE_ARGS[@]}"
  )
}

build_project() {
  local dir="$1" job_args=("${@:2}") extra=()
  if [ -n "${ARGUS_KEEP_GOING:-}" ]; then
    job_args+=(-- -k 0)
  fi
  mapfile -t extra < <(extra_targets_for "$dir")
  (
    cd "$ROOT/$dir"
    cmake --build "build/$PROFILE" "${job_args[@]}"
    if [ "${#extra[@]}" -gt 0 ]; then
      cmake --build "build/$PROFILE" --target "${extra[@]}" "${job_args[@]}"
    fi
  )
}

CURRENT_PROJECT=""
trap 'err "project failed: $CURRENT_PROJECT"' ERR

for dir in "${SELECTED[@]}"; do
  CURRENT_PROJECT="$(basename "$dir")"
  log "=== configure $CURRENT_PROJECT ($PROFILE) ==="
  configure_project "$dir"
done

if [ "$PARALLEL_PROJECTS" -le 1 ]; then
  for dir in "${SELECTED[@]}"; do
    CURRENT_PROJECT="$(basename "$dir")"
    log "=== $CURRENT_PROJECT ($PROFILE) ==="
    build_project "$dir" -j "$JOBS"
  done
else
  CURRENT_PROJECT=""
  trap - ERR
  LOG_DIR="$CONAN_OUT/logs"
  mkdir -p "$LOG_DIR"
  JOBSERVER_FIFO="$(mktemp -u "${TMPDIR:-/tmp}/argus-jobserver.XXXXXX")"
  mkfifo -m 600 "$JOBSERVER_FIFO"
  trap 'rm -f "$JOBSERVER_FIFO"' EXIT
  exec 7<>"$JOBSERVER_FIFO"
  TOKENS=$(( JOBS - PARALLEL_PROJECTS ))
  if [ "$TOKENS" -gt 0 ]; then
    head -c "$TOKENS" /dev/zero | tr '\0' '+' >&7
  fi
  declare -A RUNNING=()
  FAILED=()
  reap_one() {
    local pid name status=0
    wait -n -p pid "${!RUNNING[@]}" || status=$?
    name="${RUNNING[$pid]}"
    unset "RUNNING[$pid]"
    if [ "$status" -eq 0 ]; then
      log "--- $name built"
    else
      FAILED+=("$name")
      err "--- $name failed ($status); last lines of $LOG_DIR/$name.log:"
      tail -n 60 "$LOG_DIR/$name.log" >&2 || true
    fi
  }
  for dir in "${SELECTED[@]}"; do
    while [ "${#RUNNING[@]}" -ge "$PARALLEL_PROJECTS" ]; do reap_one; done
    name="$(basename "$dir")"
    log "=== build $name ($PROFILE) ==="
    (
      unset CMAKE_BUILD_PARALLEL_LEVEL
      export MAKEFLAGS=" -j$JOBS --jobserver-auth=fifo:$JOBSERVER_FIFO"
      build_project "$dir"
    ) > "$LOG_DIR/$name.log" 2>&1 &
    RUNNING[$!]="$name"
  done
  while [ "${#RUNNING[@]}" -gt 0 ]; do reap_one; done
  exec 7>&-
  if [ "${#FAILED[@]}" -gt 0 ]; then
    err "projects failed: ${FAILED[*]}"
    exit 1
  fi
  trap 'err "project failed: $CURRENT_PROJECT"' ERR
fi

if [ "$NO_TESTS" -eq 0 ]; then
  TEST_JOBS="${ARGUS_TEST_JOBS:-$JOBS}"
  is_job_count "$TEST_JOBS" || { err "invalid ARGUS_TEST_JOBS: $TEST_JOBS"; exit 1; }
  TEST_FAILED=()
  for dir in "${SELECTED[@]}"; do
    CURRENT_PROJECT="$(basename "$dir")"
    log "=== test $CURRENT_PROJECT ($PROFILE, $TEST_JOBS at a time) ==="
    if ! ctest --test-dir "$ROOT/$dir/build/$PROFILE" --output-on-failure -j "$TEST_JOBS"; then
      TEST_FAILED+=("$CURRENT_PROJECT")
    fi
  done
  if [ "${#TEST_FAILED[@]}" -gt 0 ]; then
    CURRENT_PROJECT="${TEST_FAILED[*]}"
    err "tests failed in: ${TEST_FAILED[*]}"
    exit 1
  fi
fi

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
