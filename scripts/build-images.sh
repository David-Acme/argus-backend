#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE="$ROOT/argus-deploy/docker-compose.yml"

SERVICES=(
  auth:argus-auth
  identity:argus-identity
  camera:argus-camera
  productivity:argus-productivity
  notification:argus-notification
  sync:argus-sync
  guard:argus-guard
  settings:argus-settings
  tts:argus-tts
  stt:argus-stt
  vlm:argus-vlm
  llm:argus-llm
  voice:argus-voice
  tunnel:argus-relay
)

BUILDER="${ARGUS_BUILD_BUILDER:-argus-build}"
CAP_GB="${ARGUS_BUILD_MEMORY_GB:-12}"
CACHE_GB="${ARGUS_BUILD_CACHE_GB:-40}"
OUT="${ARGUS_BUILD_OUT:-$ROOT/build/images}"
ONLY=""
JOBS="${ARGUS_BUILD_JOBS:-}"
LINK_POOLS="${ARGUS_BUILD_LINK_POOLS:-}"

usage() {
  cat <<'EOF'
usage: build-images.sh [--only <project>] [--memory <gb>] [--cache <gb>] [--output <dir>] [--builder <name>]

Builds the service images one at a time, each inside a buildx builder whose
own container carries a hard cgroup memory limit, so a runaway build step dies
inside that cgroup instead of taking the host with it. Per image the test
stage runs first and the shipped stage is built and tagged only when it is
green.

--only <project>  one project by its Dockerfile's --only name (auth, identity,
                  camera, productivity, notification, sync, guard, settings,
                  tts, stt, vlm, llm, voice, tunnel); default all fourteen
--jobs <n>        CMAKE_BUILD_PARALLEL_LEVEL handed to the in-image
                  build-all; empty keeps build-all's own derivation
                  ($ARGUS_BUILD_JOBS)
--link-pools <n>  ARGUS_LINK_POOLS handed to the in-image build-all; empty
                  keeps the tree's own default ($ARGUS_BUILD_LINK_POOLS)
--memory <gb>     the builder container's memory and memory-swap limit
                  (default 12, $ARGUS_BUILD_MEMORY_GB)
--cache <gb>      the builder's cache budget: every series prunes it back to
                  this many GiB first, because the builder runs with gc = false
                  (argus-deploy/buildkitd.toml) and would otherwise grow
                  without bound (default 40, $ARGUS_BUILD_CACHE_GB). Cache
                  mounts are never pruned (the prune carries
                  type!=exec.cachemount): the conan package cache lives in
                  one, and evicting it would rebuild every package from
                  source on the next series.
--output <dir>    where the per-image build logs, the per-image ctest summary
                  and build-times.tsv are written (default build/images,
                  $ARGUS_BUILD_OUT)
--builder <name>  the buildx builder to create or reuse (default argus-build,
                  $ARGUS_BUILD_BUILDER)

BuildKit ignores `docker build --memory`, and `docker compose build` accepts
no memory limit at all, so the limit is put on a `docker-container` builder's
container and every image is built with `docker buildx build --builder`. The
script refuses to start on a tree with uncommitted changes, refuses to run
beside another series, and refuses to build at all when the builder's limit
cannot be confirmed.

Each run records, in build-times.tsv, one row per image: the project, the
compose service, the image tag, the test stage's seconds and exit status, the
shipped stage's seconds and exit status, the commit, the image id and the
image size. The ctest summary of the test stage is extracted next to the log
as <service>.ctest.txt, skips included.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --only)    [ $# -ge 2 ] || { usage; exit 1; }; ONLY="$2"; shift ;;
    --jobs)    [ $# -ge 2 ] || { usage; exit 1; }; JOBS="$2"; shift ;;
    --link-pools) [ $# -ge 2 ] || { usage; exit 1; }; LINK_POOLS="$2"; shift ;;
    --memory)  [ $# -ge 2 ] || { usage; exit 1; }; CAP_GB="$2"; shift ;;
    --cache)   [ $# -ge 2 ] || { usage; exit 1; }; CACHE_GB="$2"; shift ;;
    --output)  [ $# -ge 2 ] || { usage; exit 1; }; OUT="$2"; shift ;;
    --builder) [ $# -ge 2 ] || { usage; exit 1; }; BUILDER="$2"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'build-images: unknown argument: %s\n' "$1" >&2; usage; exit 1 ;;
  esac
  shift
done

case "$CAP_GB" in
  ""|*[!0-9]*) printf 'build-images: --memory takes a whole number of GiB\n' >&2; exit 1 ;;
esac
[ "$CAP_GB" -ge 1 ] || { printf 'build-images: --memory must be at least 1\n' >&2; exit 1; }
case "$CACHE_GB" in
  ""|*[!0-9]*) printf 'build-images: --cache takes a whole number of GiB\n' >&2; exit 1 ;;
esac
[ "$CACHE_GB" -ge 1 ] || { printf 'build-images: --cache must be at least 1\n' >&2; exit 1; }
case "$JOBS" in *[!0-9]*) printf 'build-images: --jobs takes a whole number\n' >&2; exit 1 ;; esac
[ -z "$JOBS" ] || [ "$JOBS" -ge 1 ] || { printf 'build-images: --jobs must be at least 1\n' >&2; exit 1; }
case "$LINK_POOLS" in *[!0-9]*) printf 'build-images: --link-pools takes a whole number\n' >&2; exit 1 ;; esac
[ -z "$LINK_POOLS" ] || [ "$LINK_POOLS" -ge 1 ] || { printf 'build-images: --link-pools must be at least 1\n' >&2; exit 1; }

EXTRA_ARGS=()
if [ -n "$JOBS" ]; then
  EXTRA_ARGS+=(--build-arg "CMAKE_BUILD_PARALLEL_LEVEL=$JOBS")
fi
if [ -n "$LINK_POOLS" ]; then
  EXTRA_ARGS+=(--build-arg "ARGUS_LINK_POOLS=$LINK_POOLS")
fi

builder_memory_bytes() {
  docker inspect --format '{{.HostConfig.Memory}}' "buildx_buildkit_${BUILDER}0" 2>/dev/null
}

image_of() {
  docker compose -f "$COMPOSE" --profile tunnel config --format json 2>/dev/null \
    | python3 -c 'import json, sys; print(json.load(sys.stdin)["services"][sys.argv[1]]["image"])' "$1"
}

ensure_builder() {
  local want=$(( CAP_GB * 1073741824 )) have
  if docker buildx inspect "$BUILDER" >/dev/null 2>&1; then
    docker buildx inspect --bootstrap "$BUILDER" >/dev/null 2>&1 || true
    have="$(builder_memory_bytes)"
    if [ "$have" = "$want" ]; then
      return 0
    fi
    printf 'build-images: builder %s carries memory %s, wanted %s; raising it in place\n' \
      "$BUILDER" "${have:-unreadable}" "$want" >&2
    docker update --memory "${CAP_GB}g" --memory-swap "${CAP_GB}g" \
      "buildx_buildkit_${BUILDER}0" >/dev/null 2>&1 || true
    have="$(builder_memory_bytes)"
    if [ "$have" = "$want" ]; then
      return 0
    fi
    printf 'build-images: the in-place update did not take; recreating %s with --keep-state\n' \
      "$BUILDER" >&2
    docker buildx rm --keep-state "$BUILDER" >/dev/null 2>&1 || true
  fi
  docker buildx create --name "$BUILDER" --driver docker-container \
    --driver-opt "memory=${CAP_GB}g" --driver-opt "memory-swap=${CAP_GB}g" \
    --buildkitd-config "$ROOT/argus-deploy/buildkitd.toml" >/dev/null
  docker buildx inspect --bootstrap "$BUILDER" >/dev/null
  have="$(builder_memory_bytes)"
  if [ "$have" != "$want" ]; then
    printf 'build-images: builder %s carries memory %s, not %s; refusing to build without a confirmed limit\n' \
      "$BUILDER" "${have:-unreadable}" "$want" >&2
    exit 3
  fi
}

prune_cache() {
  local reclaimed
  reclaimed="$(docker buildx prune --builder "$BUILDER" --force \
    --max-used-space "${CACHE_GB}g" --filter 'type!=exec.cachemount' | tail -n 1)"
  printf 'build-images: builder %s cache budget %s GiB; %s\n' "$BUILDER" "$CACHE_GB" "$reclaimed"
}

ctest_summary() {
  local log="$1" out="$2"
  if grep -aE "tests passed|tests failed|The following tests did not run|\(Skipped\)|\*\*\*Skipped" \
      "$log" > "$out" 2>/dev/null; then
    return 0
  fi
  printf 'no ctest summary line in the log for this stage\n' > "$out"
}

main() {
  local commit failed=0 entry project service dockerfile image log
  local test_start test_rc test_seconds build_start build_rc build_seconds image_id image_size
  local lock
  mkdir -p "$OUT"
  exec {lock}>"$OUT/build-images.lock"
  flock -n "$lock" || {
    printf 'build-images: another series holds %s\n' "$OUT/build-images.lock" >&2
    exit 75
  }

  cd "$ROOT"
  if [ -n "$(git status --porcelain)" ]; then
    printf 'build-images: the tree has uncommitted changes; the images would not be the commit\n' >&2
    exit 2
  fi
  commit="$(git rev-parse HEAD)"
  ensure_builder
  prune_cache
  printf '%s\n' "$commit" > "$OUT/built-commit.txt"
  printf 'project\tservice\timage\ttest_seconds\ttest_rc\tbuild_seconds\tbuild_rc\tcommit\timage_id\tsize\n' \
    > "$OUT/build-times.tsv"
  printf 'build-images: commit %s, builder %s limited to %s GiB, output %s\n' \
    "$commit" "$BUILDER" "$CAP_GB" "$OUT"

  for entry in "${SERVICES[@]}"; do
    project="${entry%%:*}"
    service="${entry##*:}"
    if [ -n "$ONLY" ] && [ "$project" != "$ONLY" ]; then
      continue
    fi
    dockerfile="$ROOT/services/$project/Dockerfile"
    image="$(image_of "$service" || true)"
    log="$OUT/$service.log"
    test_seconds=0; test_rc=0; build_seconds=0; build_rc=-1; image_id=""; image_size=""
    if [ -z "$image" ] || [ ! -f "$dockerfile" ]; then
      printf 'build-images: %s has no image name or no %s\n' "$service" "$dockerfile" >&2
      failed=$(( failed + 1 ))
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$project" "$service" "${image:-}" 0 1 0 -1 "$commit" "" "" >> "$OUT/build-times.tsv"
      continue
    fi

    test_start="$(date +%s)"
    set +e
    docker buildx build --builder "$BUILDER" --file "$dockerfile" --target test \
      --output type=cacheonly "${EXTRA_ARGS[@]}" "$ROOT" > "$log" 2>&1
    test_rc=$?
    set -e
    test_seconds=$(( $(date +%s) - test_start ))
    ctest_summary "$log" "$OUT/$service.ctest.txt"
    if [ "$test_rc" -ne 0 ]; then
      printf 'build-images: %s test stage failed (rc %s) after %ss; the shipped image is not built\n' \
        "$service" "$test_rc" "$test_seconds" >&2
      failed=$(( failed + 1 ))
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$project" "$service" "$image" "$test_seconds" "$test_rc" 0 -1 "$commit" "" "" \
        >> "$OUT/build-times.tsv"
      continue
    fi

    build_start="$(date +%s)"
    set +e
    docker buildx build --builder "$BUILDER" --file "$dockerfile" --target runtime \
      --load --tag "$image" "${EXTRA_ARGS[@]}" "$ROOT" >> "$log" 2>&1
    build_rc=$?
    set -e
    build_seconds=$(( $(date +%s) - build_start ))
    if [ "$build_rc" -eq 0 ]; then
      image_id="$(docker image inspect --format '{{.Id}}' "$image" 2>/dev/null | cut -c1-19)"
      image_size="$(docker image inspect --format '{{.Size}}' "$image" 2>/dev/null)"
    else
      failed=$(( failed + 1 ))
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$project" "$service" "$image" "$test_seconds" "$test_rc" "$build_seconds" "$build_rc" \
      "$commit" "$image_id" "$image_size" >> "$OUT/build-times.tsv"
    printf 'build-images: %s test=%ss rc=%s build=%ss rc=%s id=%s size=%s\n' \
      "$service" "$test_seconds" "$test_rc" "$build_seconds" "$build_rc" \
      "${image_id:-none}" "${image_size:-none}"
  done

  printf 'build-images: series done, %s failure(s), commit %s\n' "$failed" "$commit"
  [ "$failed" -eq 0 ] || exit 1
}

main "$@"
