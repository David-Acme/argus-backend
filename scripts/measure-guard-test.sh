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

REPO="$TMP/repo"
git init -q "$REPO" || fail "git init"
git -C "$REPO" config user.email measure-guard-test@argus
git -C "$REPO" config user.name measure-guard-test
printf 'build/\n' > "$REPO/.gitignore"
printf 'a\n' > "$REPO/file.txt"
git -C "$REPO" add .gitignore file.txt
git -C "$REPO" commit -qm base || fail "git commit"

BUILD="$REPO/services/demo/build/prod"
mkdir -p "$BUILD"
printf 'CMAKE_BUILD_TYPE:STRING=Debug\n' > "$BUILD/CMakeCache.txt"
printf 'x\n' > "$BUILD/artifact"
printf '#!/bin/bash\nprintf "%%s\\n" "$*" > "$1"\n' > "$TMP/echo-command"
chmod +x "$TMP/echo-command"

out="$("$GUARD" --build "$BUILD" --config gates.json -- "$TMP/echo-command" "$TMP/ran" 2>&1)"
code="$?"
[ "$code" = 0 ] || fail "a clean, current build was refused: $out"
grep -Fq "build_type=Debug" <<<"$out" || fail "the key does not print the build type: $out"
grep -Fq "profile=prod" <<<"$out" || fail "the key does not print the profile: $out"
grep -Fq "config=gates.json" <<<"$out" || fail "the key does not print the config: $out"
[ -f "$TMP/ran" ] || fail "the command did not run"

printf 'b\n' > "$REPO/file.txt"
out="$("$GUARD" --build "$BUILD" -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "an uncommitted change was not refused (exit $code): $out"
grep -Fq "uncommitted changes" <<<"$out" || fail "the refusal does not name the cause: $out"
printf 'a\n' > "$REPO/file.txt"

HEAD_TS="$(git -C "$REPO" log -1 --format=%ct)"
touch -d "@$((HEAD_TS - 120))" "$BUILD/CMakeCache.txt" "$BUILD/artifact"
out="$("$GUARD" --build "$BUILD" -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "a build that predates HEAD was not refused (exit $code): $out"
grep -Fq "predates HEAD" <<<"$out" || fail "the refusal does not name the staleness: $out"
touch "$BUILD/CMakeCache.txt" "$BUILD/artifact"

DEV="$REPO/services/demo/build/dev"
mkdir -p "$DEV"
printf 'CMAKE_BUILD_TYPE:STRING=Release\n' > "$DEV/CMakeCache.txt"
printf 'x\n' > "$DEV/artifact"
out="$("$GUARD" --build "$DEV" -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "a dev build ran without an explicit profile (exit $code): $out"
grep -Fq "profile dev" <<<"$out" || fail "the refusal does not ask for --profile dev: $out"
out="$("$GUARD" --build "$DEV" --profile dev -- true 2>&1)"
code="$?"
[ "$code" = 0 ] || fail "an explicit dev profile was refused: $out"
grep -Fq "profile=dev" <<<"$out" || fail "the key does not print the dev profile: $out"

out="$("$GUARD" --build "$TMP/absent" -- true 2>&1)"
code="$?"
[ "$code" = 2 ] || fail "a missing build directory was not refused (exit $code): $out"

echo "measure-guard: ok"
