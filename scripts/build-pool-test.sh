#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HELPER="$ROOT/cmake/argus-module.cmake"

for tool in cmake ninja; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIPPED: build-pool-test: $tool is not on PATH"
    exit 77
  fi
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PROJ="$TMP/probe"
mkdir -p "$PROJ/lib"
printf 'int probe_value(){return 0;}\n' > "$PROJ/lib/probe.cc"
printf 'int probe_value();\nint main(){return probe_value();}\n' > "$PROJ/main.cc"
printf 'argus_module(NAME probe SOURCES probe.cc)\n' > "$PROJ/lib/CMakeLists.txt"
cat > "$PROJ/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(argus-pool-probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
include("$HELPER")
add_subdirectory(lib)
argus_service(NAME argus-probe MAIN main.cc MODULES argus_probe)
EOF

configure() {
  local build="$1"; shift
  cmake -S "$PROJ" -B "$build" -G Ninja "$@" > "$build.configure.log" 2>&1
}

edge_has_pool() {
  awk -v target="$2" '
    /^build / { t=$2; sub(/:$/, "", t); active=(t == target) }
    active && $1 == "pool" && $2 == "=" && $3 == "link" { hit=1 }
    END { exit !hit }
  ' "$1"
}

compile_edge_pooled() {
  awk '
    /^build / { rule=$3; pooled=0 }
    $1 == "pool" && $2 == "=" && $3 == "link" { pooled=1 }
    pooled && rule !~ /LINKER/ { bad=1 }
    END { exit (bad ? 0 : 1) }
  ' "$1"
}

depth_of() {
  awk '$1 == "depth" { print $3 }' "$1/CMakeFiles/rules.ninja"
}

meminfo_mb() {
  awk -v key="$1:" '$1 == key { printf "%d", $2 / 1024 }' /proc/meminfo
}

cgroup_memory_max() {
  local rel
  rel="$(awk -F: '$1 == "0" { print $3 }' /proc/self/cgroup 2>/dev/null)"
  cat "/sys/fs/cgroup${rel%/}/memory.max" 2>/dev/null
}

link_budget_mb() {
  local mold major minor
  mold="$(command -v mold 2>/dev/null || true)"
  if [ -z "$mold" ] || [ ! -x "$mold" ]; then
    printf '4096'
    return
  fi
  major="$(cmake --version | awk 'NR==1 && $1 == "cmake" && $2 == "version" { split($3, v, "."); print v[1] }')"
  minor="$(cmake --version | awk 'NR==1 && $1 == "cmake" && $2 == "version" { split($3, v, "."); print v[2] }')"
  if [ "${major:-0}" -gt 3 ] || { [ "${major:-0}" -eq 3 ] && [ "${minor:-0}" -ge 29 ]; }; then
    printf '3072'
  else
    printf '4096'
  fi
}

expect_depth() {
  local build="$1" want="$2"
  local got
  got="$(depth_of "$build")"
  if [ "$got" != "$want" ]; then
    echo "build-pool-test: $3 gave link depth $got, not $want" >&2
    exit 1
  fi
}

budget="$(link_budget_mb)"
case "$budget" in
  3072) want16=5; want14=4; want18=6 ;;
  4096) want16=4; want14=3; want18=4 ;;
  *) echo "build-pool-test: unexpected link budget $budget MiB" >&2; exit 1 ;;
esac

configure "$TMP/build16" -DARGUS_BUILD_MEMORY_CAP_MB=16384
if ! edge_has_pool "$TMP/build16/build.ninja" argus-probe; then
  echo "build-pool-test: the executable link edge carries no pool = link" >&2
  exit 1
fi
if compile_edge_pooled "$TMP/build16/build.ninja"; then
  echo "build-pool-test: a compile edge carries the link pool" >&2
  exit 1
fi
if ! grep -q '^  pool = link$' "$TMP/build16/build.ninja"; then
  echo "build-pool-test: no pool = link binding in build.ninja" >&2
  exit 1
fi
expect_depth "$TMP/build16" "$want16" "a 16384 MiB cap"

configure "$TMP/build14" -DARGUS_BUILD_MEMORY_CAP_MB=14336
expect_depth "$TMP/build14" "$want14" "a 14336 MiB cap"

configure "$TMP/build18" -DARGUS_BUILD_MEMORY_CAP_MB=18432
expect_depth "$TMP/build18" "$want18" "a 18432 MiB cap"

configure "$TMP/build2" -DARGUS_LINK_POOLS=2
expect_depth "$TMP/build2" 2 "ARGUS_LINK_POOLS=2"

configure "$TMP/build1" -DARGUS_BUILD_MEMORY_CAP_MB=1000
expect_depth "$TMP/build1" 1 "a 1000 MiB cap"

neutralised=1
if systemd-run --user --scope --quiet -p MemoryMax=infinity -- true >/dev/null 2>&1; then
  configure_unscoped() {
    local build="$1"; shift
    systemd-run --user --scope --quiet -p MemoryMax=infinity -- \
      cmake -S "$PROJ" -B "$build" -G Ninja "$@" > "$build.configure.log" 2>&1
  }
else
  configure_unscoped() {
    local build="$1"; shift
    cmake -S "$PROJ" -B "$build" -G Ninja "$@" > "$build.configure.log" 2>&1
  }
  cgmax="$(cgroup_memory_max)"
  if [ -n "$cgmax" ] && [ "$cgmax" != "max" ]; then
    neutralised=0
  fi
fi

if [ "$neutralised" -eq 1 ]; then
  avail_before="$(meminfo_mb MemAvailable)"
  configure_unscoped "$TMP/buildfallback"
  avail_after="$(meminfo_mb MemAvailable)"
  lo="$avail_before"; [ "$avail_after" -lt "$lo" ] && lo="$avail_after"
  hi="$avail_before"; [ "$avail_after" -gt "$hi" ] && hi="$avail_after"
  want_lo=$(( (lo - 4096) / budget )); [ "$want_lo" -lt 1 ] && want_lo=1
  want_hi=$(( (hi - 4096) / budget )); [ "$want_hi" -lt 1 ] && want_hi=1
  got="$(depth_of "$TMP/buildfallback")"
  if [ "$got" -lt "$want_lo" ] || [ "$got" -gt "$want_hi" ]; then
    echo "build-pool-test: no-cap fallback depth $got is outside the MemAvailable range $want_lo..$want_hi" >&2
    exit 1
  fi
  total_mb="$(meminfo_mb MemTotal)"
  want_total=$(( (total_mb - 4096) / budget )); [ "$want_total" -lt 1 ] && want_total=1
  if [ "$want_total" -ne "$want_lo" ] && [ "$want_total" -ne "$want_hi" ] && [ "$got" -eq "$want_total" ]; then
    echo "build-pool-test: the no-cap fallback used MemTotal, not MemAvailable (depth $got)" >&2
    exit 1
  fi
else
  echo "build-pool-test: no-cap fallback not exercised (cgroup capped, systemd-run unavailable)" >&2
fi

echo "build-pool-test: pool = link on link edges (budget ${budget} MiB: 16384 -> $want16, 14336 -> $want14, 18432 -> $want18, override 2, 1000 -> 1, fallback from MemAvailable)"
