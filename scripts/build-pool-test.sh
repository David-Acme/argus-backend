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
  local ninja_file="$1" target="$2"
  awk -v target="$target" '
    /^build / { t=$2; sub(/:$/, "", t); active=(t == target) }
    active && $1 == "pool" && $2 == "=" && $3 == "link" { hit=1 }
    END { exit !hit }
  ' "$ninja_file"
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

configure "$TMP/build14" -DARGUS_BUILD_MEMORY_CAP_MB=14336
if ! edge_has_pool "$TMP/build14/build.ninja" argus-probe; then
  echo "build-pool-test: the executable link edge carries no pool = link" >&2
  exit 1
fi
if compile_edge_pooled "$TMP/build14/build.ninja"; then
  echo "build-pool-test: a compile edge carries the link pool" >&2
  exit 1
fi
if ! grep -q '^  pool = link$' "$TMP/build14/build.ninja"; then
  echo "build-pool-test: no pool = link binding in build.ninja" >&2
  exit 1
fi
depth="$(depth_of "$TMP/build14")"
if [ "$depth" != 4 ]; then
  echo "build-pool-test: a 14336 MiB cap gave link depth $depth, not 4" >&2
  exit 1
fi

configure "$TMP/build2" -DARGUS_LINK_POOLS=2
test "$(depth_of "$TMP/build2")" = 2

configure "$TMP/build1" -DARGUS_BUILD_MEMORY_CAP_MB=1000
test "$(depth_of "$TMP/build1")" = 1

configure "$TMP/builddefault"
if ! edge_has_pool "$TMP/builddefault/build.ninja" argus-probe; then
  echo "build-pool-test: the default configure carries no pool = link" >&2
  exit 1
fi
default_depth="$(depth_of "$TMP/builddefault")"
case "$default_depth" in
  ""|*[!0-9]*) echo "build-pool-test: the default configure has no link depth" >&2
               exit 1 ;;
esac
test "$default_depth" -ge 1

echo "build-pool-test: pool = link on link edges (cap 14336 MiB -> depth 4, override 2, floor 1)"
