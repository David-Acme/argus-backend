#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

CALL_LOG="$TEST_TMP/calls.log"
MOCK_BIN="$TEST_TMP/bin"
mkdir -p "$MOCK_BIN"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s %s\n" "$(basename "$0")" "$*" >> "$ARGUS_BUILD_ALL_TEST_LOG"' \
  > "$MOCK_BIN/tool"
chmod +x "$MOCK_BIN/tool"
ln -s tool "$MOCK_BIN/conan"
ln -s tool "$MOCK_BIN/cmake"
ln -s tool "$MOCK_BIN/ctest"

run_build_all() {
  : > "$CALL_LOG"
  PATH="$MOCK_BIN:$PATH" ARGUS_BUILD_ALL_TEST_LOG="$CALL_LOG" \
    "$ROOT/scripts/build-all.sh" "$@"
}

run_build_all dev --only cert --install-only
grep -q '^conan install ' "$CALL_LOG"
if grep -Eq '^(cmake|ctest) ' "$CALL_LOG"; then
  echo "--install-only invoked a build or test command" >&2
  exit 1
fi

run_build_all prod --only cert --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 2
if grep -q '^ctest ' "$CALL_LOG"; then
  echo "--no-tests invoked ctest" >&2
  exit 1
fi

run_build_all dev --only identity --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 3
grep -q '^cmake --build build/dev -j 8 --target argus-migrate-identity$' "$CALL_LOG"

run_build_all prod --only camera --no-tests
test "$(grep -c '^cmake ' "$CALL_LOG")" -eq 3
grep -q '^cmake --build build/prod -j 8 --target argus-migrate-camera argus-vulkan-probe$' "$CALL_LOG"

# One dependency resolution for the whole tree, against the root manifest, and
# no per-project presets: section 2.6's single manifest is what this locks.
run_build_all dev --only camera
test "$(grep -c '^conan install ' "$CALL_LOG")" -eq 1
grep -Fq "conan install $ROOT --output-folder=$ROOT/build/dev -s build_type=Debug --build=missing" "$CALL_LOG"
test "$(grep -c '^ctest ' "$CALL_LOG")" -eq 1
grep -Fq "cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=$ROOT/build/dev/build/Debug/generators/conan_toolchain.cmake -DCMAKE_PREFIX_PATH=$ROOT/build/dev/build/Debug/generators -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" "$CALL_LOG"
if grep -q -- '--preset' "$CALL_LOG"; then
  echo "build-all still drives cmake through a per-project preset" >&2
  exit 1
fi

if run_build_all dev --only does-not-exist; then
  echo "unknown --only project unexpectedly succeeded" >&2
  exit 1
fi

# Section 2.4's tier table is checked by build-all before anything is built, and
# the check itself is exercised here. Each rejection asserts *why* it was
# rejected: a fixture that trips two rules at once would pass a status-only
# check while one of the two detectors was broken, which is what the cycle
# fixture below did before it was isolated.
grep -Fq '"$ROOT/scripts/check-deps.sh"' "$ROOT/scripts/build-all.sh"
"$ROOT/scripts/check-deps.sh" >/dev/null

FIXTURE="$TEST_TMP/dag"

expect_rejected() {          # expect_rejected <what> <message fragment> [root]
  local what="$1" fragment="$2" root="${3:-$FIXTURE}" out
  if out="$("$ROOT/scripts/check-deps.sh" --root "$root" 2>&1)"; then
    echo "check-deps passed $what" >&2
    exit 1
  fi
  case "$out" in
    *"$fragment"*) ;;
    *) echo "check-deps rejected $what for some other reason:" >&2
       printf '%s\n' "$out" >&2
       exit 1 ;;
  esac
}

# The legal tree every case starts from: a lib, a service that links it and a
# client that links the lib. Each case rewrites one file, so a fixture can only
# fail for the rule it is about.
write_fixture() {
  rm -rf "$FIXTURE"
  mkdir -p "$FIXTURE/packages/lib/y" "$FIXTURE/packages/clients/x" \
    "$FIXTURE/services/s"
  printf 'argus_lib(NAME y\n    DEPENDS\n        Drogon::Drogon)\n' \
    > "$FIXTURE/packages/lib/y/CMakeLists.txt"
  printf 'argus_service(NAME s MAIN main.cc DEPENDS argus::lib::y)\nargus_module(NAME s)\n' \
    > "$FIXTURE/services/s/CMakeLists.txt"
  printf 'argus_clients(NAME x\n    DEPENDS\n        argus::lib::y)\n' \
    > "$FIXTURE/packages/clients/x/CMakeLists.txt"
}

write_fixture
"$ROOT/scripts/check-deps.sh" --root "$FIXTURE" >/dev/null

printf 'argus_clients(NAME x\n    DEPENDS\n        argus::s)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a tier-3 -> tier-5 edge" "forbidden: T3 -> T5"

# A semicolon separates list items in CMake exactly as a newline does, so the
# same edge written that way has to be judged the same way.
printf 'argus_clients(NAME x\n    DEPENDS\n        argus::lib::y;argus::s)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a tier-3 -> tier-5 edge in a semicolon list" "forbidden: T3 -> T5"

# A bare service name is the spelling a tier-3 -> tier-5 edge is written in, and
# argus_service creates that target inside the helper: the package's own file
# never says add_executable, so the check has to register it to see the edge.
printf 'argus_clients(NAME x\n    DEPENDS\n        s)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a tier-3 -> tier-5 edge written as a bare target" \
  "forbidden: T3 -> T5"

# A cycle is a rule-1 violation on its own, so this fixture carries nothing
# else: two tier-1 packages pointing at each other, and no edge the table could
# object to.
write_fixture
mkdir -p "$FIXTURE/packages/lib/z"
printf 'argus_lib(NAME y\n    DEPENDS\n        argus::lib::z)\n' \
  > "$FIXTURE/packages/lib/y/CMakeLists.txt"
printf 'argus_lib(NAME z\n    DEPENDS\n        argus::lib::y)\n' \
  > "$FIXTURE/packages/lib/z/CMakeLists.txt"
expect_rejected "a cycle between two packages" "forbidden: cycle"

# A cycle is a rule-1 violation whatever tier its ends have, so it is found
# through a package section 9.1 has not moved yet (which has no tier at all).
write_fixture
mkdir -p "$FIXTURE/packages/memory"
printf 'argus_lib(NAME y\n    DEPENDS\n        argus::lib::memory)\n' \
  > "$FIXTURE/packages/lib/y/CMakeLists.txt"
printf 'argus_lib(NAME memory\n    DEPENDS\n        argus::lib::y)\n' \
  > "$FIXTURE/packages/memory/CMakeLists.txt"
expect_rejected "a cycle through a package with no tier" "forbidden: cycle"

# The first-party namespace is this tree's own, so a name it does not declare is
# a typo: counting it as a foreign library would hide the edge it was meant to
# declare, and the typo would pass the gate.
write_fixture
printf 'argus_clients(NAME x\n    DEPENDS\n        argus::lib::yy)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a dependency on a name this tree does not declare" \
  "unresolved: packages/clients/x names argus::lib::yy"

# A run that examined nothing is not a run that passed: every count is zero, and
# zero reads exactly like a clean tree.
EMPTY_FIXTURE="$TEST_TMP/dag-empty"
mkdir -p "$EMPTY_FIXTURE/packages"
expect_rejected "a tree with no CMakeLists.txt" "nothing was checked" \
  "$EMPTY_FIXTURE"
mkdir -p "$EMPTY_FIXTURE/packages/lib/w"
printf 'project(w)\n' > "$EMPTY_FIXTURE/packages/lib/w/CMakeLists.txt"
expect_rejected "a tree with no argus_* declaration" "nothing was checked" \
  "$EMPTY_FIXTURE"

# Rules 16 and 19 are measured by the full gate only: the scan needs every
# project's compile database, and a --only run must stay fast.
grep -Fq '"$ROOT/scripts/check-tidy.sh"' "$ROOT/scripts/build-all.sh"
if run_build_all dev --only camera 2>&1 | grep -q 'rules 16 and 19'; then
  echo "a --only run reached the clang-tidy gate" >&2
  exit 1
fi

# A finding count belongs to the clang-tidy that produced it, so the baseline
# records its version and the scan refuses to compare across majors; the
# versioned binary LLVM's packages install is found when the plain name is
# missing, which is how CI gets one.
baseline_major="$(sed -n 's/^tool \([0-9]*\)\..*/\1/p' \
  "$ROOT/scripts/lib/tidy-baseline.txt")"
tidy_tool="clang-tidy"
if ! command -v "$tidy_tool" >/dev/null 2>&1; then
  tidy_tool="clang-tidy-$baseline_major"
fi
if command -v "$tidy_tool" >/dev/null 2>&1; then
  TIDY="$TEST_TMP/tidy"
  mkdir -p "$TIDY/packages/lib/z/src" "$TIDY/packages/lib/z/build/dev" \
    "$TIDY/scripts/lib"
  printf 'Checks: >\n  -*,\n  modernize-*,\n  -modernize-use-trailing-return-type\n' \
    > "$TIDY/.clang-tidy"
  printf 'int f() { return 0; }\n' > "$TIDY/packages/lib/z/src/z.cc"
  # The second entry names a file the fixture does not have: a compile database
  # can outlive its files (a build tree left behind by a rename), and such an
  # entry is skipped and reported, never handed to clang-tidy.
  tidy_cxx="$(command -v c++ || command -v g++ || echo c++)"
  printf '[{"directory": "%s/packages/lib/z/build/dev", "command": "%s -std=c++20 -c %s/packages/lib/z/src/z.cc -o %s/packages/lib/z/build/dev/z.o", "file": "%s/packages/lib/z/src/z.cc"},\n {"directory": "%s/packages/lib/z/build/dev", "command": "%s -std=c++20 -c %s/packages/lib/z/src/gone.cc -o %s/packages/lib/z/build/dev/gone.o", "file": "%s/packages/lib/z/src/gone.cc"}]\n' \
    "$TIDY" "$tidy_cxx" "$TIDY" "$TIDY" "$TIDY" "$TIDY" "$tidy_cxx" "$TIDY" \
    "$TIDY" "$TIDY" > "$TIDY/packages/lib/z/build/dev/compile_commands.json"
  major="$("$tidy_tool" --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p' \
    | head -1)"

  printf 'tool %s.99.99\ntus 1\n' "$major" \
    > "$TIDY/scripts/lib/tidy-baseline.txt"
  if ! "$ROOT/scripts/check-tidy.sh" --root "$TIDY" >/dev/null 2>&1; then
    echo "check-tidy refused a baseline measured with its own major" >&2
    exit 1
  fi

  printf 'tool 1.0.0\ntus 1\n' > "$TIDY/scripts/lib/tidy-baseline.txt"
  if "$ROOT/scripts/check-tidy.sh" --root "$TIDY" >/dev/null 2>&1; then
    echo "check-tidy compared counts across clang-tidy majors" >&2
    exit 1
  fi

  if ! "$ROOT/scripts/check-tidy.sh" --root "$TIDY" --write-baseline >/dev/null; then
    echo "check-tidy could not record a baseline" >&2
    exit 1
  fi
  grep -q "^tool $major\." "$TIDY/scripts/lib/tidy-baseline.txt" || {
    echo "the recorded baseline does not name the tool that measured it" >&2
    exit 1
  }
  if ! "$ROOT/scripts/check-tidy.sh" --root "$TIDY" >/dev/null; then
    echo "check-tidy is not green against the baseline it recorded" >&2
    exit 1
  fi
  if ! tidy_output="$("$ROOT/scripts/check-tidy.sh" --root "$TIDY" 2>&1)"; then
    echo "check-tidy failed where a stale entry should be skipped" >&2
    exit 1
  fi
  case "$tidy_output" in
    *"entries skipped in"*) ;;
    *) echo "check-tidy did not report the stale database entry" >&2
       exit 1 ;;
  esac

  # A baseline measured over a tree clang-tidy could not read is worse than no
  # baseline: it would record fewer TUs and fewer findings than the tree has,
  # and every later run would compare against a floor that is too low. The
  # broken entry here is a legal file whose compile command names a header that
  # is not there, which is what a stale toolchain path looks like.
  cp "$TIDY/scripts/lib/tidy-baseline.txt" "$TEST_TMP/baseline.before"
  printf '[{"directory": "%s/packages/lib/z/build/dev", "command": "%s -std=c++20 -include %s/packages/lib/z/src/missing.h -c %s/packages/lib/z/src/z.cc -o %s/packages/lib/z/build/dev/z.o", "file": "%s/packages/lib/z/src/z.cc"}]\n' \
    "$TIDY" "$tidy_cxx" "$TIDY" "$TIDY" "$TIDY" "$TIDY" \
    > "$TIDY/packages/lib/z/build/dev/compile_commands.json"
  if tidy_output="$("$ROOT/scripts/check-tidy.sh" --root "$TIDY" --write-baseline 2>&1)"; then
    echo "check-tidy recorded a baseline over a TU it could not read" >&2
    exit 1
  fi
  case "$tidy_output" in
    *"could not be analysed"*) ;;
    *) echo "check-tidy did not say why it refused to write a baseline" >&2
       printf '%s\n' "$tidy_output" >&2
       exit 1 ;;
  esac
  if ! cmp -s "$TEST_TMP/baseline.before" "$TIDY/scripts/lib/tidy-baseline.txt"; then
    echo "check-tidy wrote a baseline it said it would not write" >&2
    exit 1
  fi
else
  echo "check-tidy tests skipped: no clang-tidy on PATH" >&2
fi

grep -Fq '"$ROOT/scripts/build-all.sh" "$PROFILE" --install-only' \
  "$ROOT/scripts/setup.sh"
grep -Fq '"$ROOT/scripts/build-all.sh" "$PROFILE"' \
  "$ROOT/scripts/setup.sh"

grep -Fq 'git rev-parse --is-inside-work-tree' "$ROOT/scripts/setup.sh"
if grep -Fq '[ ! -d ".git" ]' "$ROOT/scripts/setup.sh"; then
  echo "setup still rejects linked Git worktrees" >&2
  exit 1
fi

echo "build-all tests passed"
