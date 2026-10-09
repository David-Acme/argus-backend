#!/bin/bash
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUARD="$ROOT/scripts/measure-guard.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
  printf 'measure-guard-test: %s\n' "$1" >&2
  exit 1
}

command -v cmake >/dev/null 2>&1 || { echo "SKIPPED: no cmake"; exit 77; }
command -v ninja >/dev/null 2>&1 || { echo "SKIPPED: no ninja"; exit 77; }

REPO="$TMP/repo"
BUILD="$REPO/build/dev"
SOURCE="$REPO/src/main.cc"

git init -q "$REPO" || fail "git init"
git -C "$REPO" config user.email measure-guard-test@argus
git -C "$REPO" config user.name measure-guard-test
mkdir -p "$TMP/repo/src"
printf 'build/\n' > "$REPO/.gitignore"
printf 'cmake_minimum_required(VERSION 3.16)\nproject(probe CXX)\nadd_executable(probe src/main.cc)\n' > "$REPO/CMakeLists.txt"
printf 'int main() { return 0; }\n' > "$SOURCE"
git -C "$REPO" add .gitignore CMakeLists.txt src/main.cc
git -C "$REPO" commit -qm base || fail "git commit"

cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1 || fail "configure"
cmake --build "$BUILD" --target probe >/dev/null 2>&1 || fail "build"

guard() {
  local out code
  out="$("$GUARD" --build "$BUILD" --profile dev --target probe --config gates.json -- true 2>&1)"
  code="$?"
  printf '%s\n%s\n' "$code" "$out"
}

out="$(guard)"
code="$(head -1 <<<"$out")"
[ "$code" = 0 ] || fail "a clean, built tree was refused: $(tail -n +2 <<<"$out")"
grep -Fq "build_type=Debug" <<<"$out" || fail "the key does not print the build type: $out"
grep -Fq "profile=dev" <<<"$out" || fail "the key does not print the profile: $out"
grep -Fq "config=gates.json" <<<"$out" || fail "the key does not print the config: $out"

printf 'int main() { return 1; }\n' > "$SOURCE"
cmake --build "$BUILD" --target probe >/dev/null 2>&1 || fail "rebuild with the edit"
sleep 1
printf 'int main() { return 0; }\n' > "$SOURCE"
out="$(guard)"
code="$(head -1 <<<"$out")"
[ "$code" = 2 ] || fail "a reverted edit was not refused (exit $code): $(tail -n +2 <<<"$out")"
grep -Fq "newer than" <<<"$out" || fail "the refusal does not name the staleness: $out"

sleep 1
cmake --build "$BUILD" --target probe >/dev/null 2>&1 || fail "rebuild after the revert"
out="$(guard)"
code="$(head -1 <<<"$out")"
[ "$code" = 0 ] || fail "a rebuilt tree was refused: $(tail -n +2 <<<"$out")"

printf 'int main() { return 2; }\n' > "$SOURCE"
out="$(guard)"
code="$(head -1 <<<"$out")"
[ "$code" = 2 ] || fail "an uncommitted change was not refused (exit $code): $(tail -n +2 <<<"$out")"
grep -Fq "uncommitted changes" <<<"$out" || fail "the refusal does not name the cause: $out"
printf 'int main() { return 0; }\n' > "$SOURCE"

out="$("$GUARD" --build "$BUILD" --target probe -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "a dev build ran without an explicit profile (exit $code): $out"
grep -Fq "profile dev" <<<"$out" || fail "the refusal does not ask for --profile dev: $out"

out="$("$GUARD" --build "$TMP/absent" --profile dev --target probe -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "a missing build directory was not refused (exit $code): $out"

out="$("$GUARD" --build "$BUILD" --profile dev -- true 2>&1)"
code="$?"
[ "$code" = 1 ] || fail "a missing --target was not a usage error (exit $code): $out"

echo "measure-guard: ok"
