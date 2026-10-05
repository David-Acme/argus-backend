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

grep -Fq '"$ROOT/scripts/check-comments.sh"' "$ROOT/scripts/build-all.sh"
"$ROOT/scripts/check-comments.sh" >/dev/null

COMMENTS="$TEST_TMP/comments"

write_clean_comment_fixture() {
  rm -rf "$COMMENTS"
  mkdir -p "$COMMENTS/src"
  git -C "$COMMENTS" init -q
  cat > "$COMMENTS/src/a.cc" <<'SRC'
#include <string>
const std::string url = "http://argus.local//x";
const char* raw = R"sql(SELECT 1 -- kept // kept)sql";
constexpr long big = 1'000'000;
const char quote = '"';
int ratio(int a, int b) { return a / b; }
SRC
  cat > "$COMMENTS/src/b.sh" <<'SRC'
#!/usr/bin/env bash
echo "#literal" '#literal' ${#1} $# a#b $(( 16#ff ))
read -r first <<< "#literal"
[[ "$first" =~ ^#x ]] || true
cat <<EOF
# heredoc body is data
EOF
SRC
  printf 'project(p)\nset(X "#literal")\n' > "$COMMENTS/src/CMakeLists.txt"
  printf "CREATE TABLE t (a TEXT DEFAULT '--literal');\n" > "$COMMENTS/src/s.sql"
  printf 'key = "#literal"\n' > "$COMMENTS/src/c.toml.example"
  printf 'a: "#literal"\nb: x#y\n' > "$COMMENTS/src/d.yml"
  printf '# syntax=docker/dockerfile:1.7\nFROM scratch\n' > "$COMMENTS/src/Dockerfile"
  printf '#!/usr/bin/env python3\nx = "#literal"\n' > "$COMMENTS/src/e.py"
  printf 'syntax = "proto3";\nmessage M { string url = 1; }\n' > "$COMMENTS/src/f.proto"
  printf '# notes\n' > "$COMMENTS/src/g.md"
  cat > "$COMMENTS/src/h.sh" <<'SRC'
#!/usr/bin/env bash
x="$(case $v in a) printf "%s" "a #b";; esac)"
echo dir\ #1
echo $'a\'b' 'x #y'
(( m = 1 << bits ))
cat <<END-OF
# data
END-OF
y=`echo a`
echo ${y:-'}'}
SRC
  cat > "$COMMENTS/src/i.yml" <<'SRC'
x: &a |
  # data line
y: !!str >
  # data
z: "multi
  #line"
SRC
  cat > "$COMMENTS/src/j.toml" <<'SRC'
a = """x\"""
# data
"""
SRC
  cat > "$COMMENTS/src/Dockerfile.heredoc" <<'SRC'
FROM scratch
RUN <<EOF
#!/usr/bin/env python3
print(1)
EOF
SRC
  printf '{"a": "http://x//y"}\n' > "$COMMENTS/src/k.json"
  printf '[submodule "x"]\n\tpath = x\n' > "$COMMENTS/src/.gitmodules"
  printf 'K="v # quoted"\nJ=v#w\n' > "$COMMENTS/src/.env.example"
}

expect_comment_rejected() {
  local what="$1" fragment="$2" out
  if out="$("$ROOT/scripts/check-comments.sh" --root "$COMMENTS" 2>&1)"; then
    echo "check-comments passed $what" >&2
    exit 1
  fi
  case "$out" in
    *"$fragment"*) ;;
    *) echo "check-comments rejected $what for some other reason:" >&2
       printf '%s\n' "$out" >&2
       exit 1 ;;
  esac
}

write_clean_comment_fixture
if ! out="$("$ROOT/scripts/check-comments.sh" --root "$COMMENTS" 2>&1)"; then
  echo "check-comments rejected code that holds no comment:" >&2
  printf '%s\n' "$out" >&2
  exit 1
fi

for case_line in \
    'a.cc|int x; // note|comment: src/a.cc:7' \
    'a.cc|/* note */|comment: src/a.cc:7' \
    'b.sh|echo x # note|comment: src/b.sh:8' \
    'CMakeLists.txt|# note|comment: src/CMakeLists.txt:3' \
    's.sql|-- note|comment: src/s.sql:2' \
    'c.toml.example|# note|comment: src/c.toml.example:2' \
    'd.yml|c: 1 # note|comment: src/d.yml:3' \
    'Dockerfile|# note|comment: src/Dockerfile:3' \
    'e.py|# note|comment: src/e.py:3' \
    'f.proto|// note|comment: src/f.proto:3'; do
  IFS='|' read -r file text fragment <<< "$case_line"
  write_clean_comment_fixture
  printf '%s\n' "$text" >> "$COMMENTS/src/$file"
  expect_comment_rejected "a comment in $file" "$fragment"
done

write_clean_comment_fixture
printf 'def f():\n    """note"""\n    return 1\n' >> "$COMMENTS/src/e.py"
expect_comment_rejected "a docstring" "comment: src/e.py:4"

write_clean_comment_fixture
printf 'print(__doc__)\n' >> "$COMMENTS/src/e.py"
expect_comment_rejected "a file that reads __doc__" "unread: src/e.py: reads __doc__"

write_clean_comment_fixture
printf 'x=`echo a #b`\n' >> "$COMMENTS/src/b.sh"
expect_comment_rejected "a comment inside backticks" "comment: src/b.sh:8"

write_clean_comment_fixture
printf 'steps:\n  - run: |\n      echo\n    # sibling\n    name: y\n' > "$COMMENTS/src/d.yml"
expect_comment_rejected "a comment after a block scalar" "comment: src/d.yml:4"

write_clean_comment_fixture
printf '; note\n' >> "$COMMENTS/src/.gitmodules"
expect_comment_rejected "a semicolon comment in .gitmodules" "comment: src/.gitmodules:3"

write_clean_comment_fixture
printf 'L=v # note\n' >> "$COMMENTS/src/.env.example"
expect_comment_rejected "an inline comment in an env file" "comment: src/.env.example:3"

write_clean_comment_fixture
printf '// note\n' >> "$COMMENTS/src/k.json"
expect_comment_rejected "a comment in a json file" "comment: src/k.json:2"

write_clean_comment_fixture
printf 'x\n' > "$COMMENTS/src/h.unknownext"
expect_comment_rejected "a file no scanner reads" "unclassified: src/h.unknownext"

rm -rf "$COMMENTS" && mkdir -p "$COMMENTS" && git -C "$COMMENTS" init -q
set +e
"$ROOT/scripts/check-comments.sh" --root "$COMMENTS" >/dev/null 2>&1
status=$?
set -e
if [ "$status" -ne 2 ]; then
  echo "check-comments passed a tree with nothing to check (exit $status)" >&2
  exit 1
fi

rm -rf "$COMMENTS" && mkdir -p "$COMMENTS/src" && printf 'int x;\n' > "$COMMENTS/src/a.cc"
set +e
out="$("$ROOT/scripts/check-comments.sh" --root "$COMMENTS" 2>&1)"
status=$?
set -e
case "$status:$out" in
  "2:"*"not a git work tree"*) ;;
  *) echo "check-comments did not refuse a tree git cannot list (exit $status):" >&2
     printf '%s\n' "$out" >&2
     exit 1 ;;
esac

NOGIT="$TEST_TMP/nogit"
mkdir -p "$NOGIT/packages/lib/y"
cp -R "$ROOT/scripts" "$NOGIT/scripts"
printf 'argus_lib(NAME y\n    DEPENDS\n        Drogon::Drogon)\n' \
  > "$NOGIT/packages/lib/y/CMakeLists.txt"
: > "$CALL_LOG"
if ! out="$(PATH="$MOCK_BIN:$PATH" ARGUS_BUILD_ALL_TEST_LOG="$CALL_LOG" \
    "$NOGIT/scripts/build-all.sh" dev --only cert --install-only 2>&1)"; then
  echo "build-all failed outside a git work tree, where an image builds:" >&2
  printf '%s\n' "$out" >&2
  exit 1
fi
case "$out" in
  *"not a git work tree"*) ;;
  *) echo "build-all did not say why it skipped the comment gate" >&2
     exit 1 ;;
esac

write_clean_comment_fixture
cp "$COMMENTS/src/a.cc" "$TEST_TMP/a.cc.clean"
printf '// header\n\nint y;  // trailing\n\n// before closer\n' >> "$COMMENTS/src/a.cc"
printf '\nint y;\n' >> "$TEST_TMP/a.cc.clean"
"$ROOT/scripts/check-comments.sh" --root "$COMMENTS" --fix >/dev/null
if ! cmp -s "$TEST_TMP/a.cc.clean" "$COMMENTS/src/a.cc"; then
  echo "check-comments --fix did not leave the expected code:" >&2
  diff "$TEST_TMP/a.cc.clean" "$COMMENTS/src/a.cc" >&2 || true
  exit 1
fi
"$ROOT/scripts/check-comments.sh" --root "$COMMENTS" >/dev/null

write_clean_comment_fixture
cat > "$COMMENTS/src/m.cc" <<'SRC'
#define FOO 1 /* a
b */ + 2
int a = 1 -/**/- 2;
int g = f(/*a=*/1);
/* c */ const char* s = R"(abc   
def)";
    /* d */ int y;
SRC
printf '#define FOO 1 + 2\nint a = 1 - - 2;\nint g = f(1);\nconst char* s = R"(abc   \ndef)";\n    int y;\n' \
  > "$TEST_TMP/m.cc.clean"
printf 'def f():\n    """doc"""; return 1\n' > "$COMMENTS/src/n.py"
printf 'def f():\n    return 1\n' > "$TEST_TMP/n.py.clean"
"$ROOT/scripts/check-comments.sh" --root "$COMMENTS" --fix >/dev/null
for fixed in m.cc n.py; do
  if ! cmp -s "$TEST_TMP/$fixed.clean" "$COMMENTS/src/$fixed"; then
    echo "check-comments --fix did not leave the expected $fixed:" >&2
    diff "$TEST_TMP/$fixed.clean" "$COMMENTS/src/$fixed" >&2 || true
    exit 1
  fi
done
"$ROOT/scripts/check-comments.sh" --root "$COMMENTS" >/dev/null

grep -Fq '"$ROOT/scripts/check-deps.sh"' "$ROOT/scripts/build-all.sh"
"$ROOT/scripts/check-deps.sh" >/dev/null

FIXTURE="$TEST_TMP/dag"

expect_rejected() {
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

printf 'argus_clients(NAME x\n    DEPENDS\n        argus::lib::y;argus::s)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a tier-3 -> tier-5 edge in a semicolon list" "forbidden: T3 -> T5"

printf 'argus_clients(NAME x\n    DEPENDS\n        s)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a tier-3 -> tier-5 edge written as a bare target" \
  "forbidden: T3 -> T5"

write_fixture
mkdir -p "$FIXTURE/packages/lib/z"
printf 'argus_lib(NAME y\n    DEPENDS\n        argus::lib::z)\n' \
  > "$FIXTURE/packages/lib/y/CMakeLists.txt"
printf 'argus_lib(NAME z\n    DEPENDS\n        argus::lib::y)\n' \
  > "$FIXTURE/packages/lib/z/CMakeLists.txt"
expect_rejected "a cycle between two packages" "forbidden: cycle"

write_fixture
mkdir -p "$FIXTURE/packages/stray"
printf 'argus_lib(NAME y\n    DEPENDS\n        argus::lib::stray)\n' \
  > "$FIXTURE/packages/lib/y/CMakeLists.txt"
printf 'argus_lib(NAME stray\n    DEPENDS\n        argus::lib::y)\n' \
  > "$FIXTURE/packages/stray/CMakeLists.txt"
expect_rejected "a cycle through a package with no tier" "forbidden: cycle"

write_fixture
printf 'argus_clients(NAME x\n    DEPENDS\n        argus::lib::yy)\n' \
  > "$FIXTURE/packages/clients/x/CMakeLists.txt"
expect_rejected "a dependency on a name this tree does not declare" \
  "unresolved: packages/clients/x names argus::lib::yy"

EMPTY_FIXTURE="$TEST_TMP/dag-empty"
mkdir -p "$EMPTY_FIXTURE/packages"
expect_rejected "a tree with no CMakeLists.txt" "nothing was checked" \
  "$EMPTY_FIXTURE"
mkdir -p "$EMPTY_FIXTURE/packages/lib/w"
printf 'project(w)\n' > "$EMPTY_FIXTURE/packages/lib/w/CMakeLists.txt"
expect_rejected "a tree with no argus_* declaration" "nothing was checked" \
  "$EMPTY_FIXTURE"

grep -Fq '"$ROOT/scripts/check-tidy.sh"' "$ROOT/scripts/build-all.sh"
if run_build_all dev --only camera 2>&1 | grep -q 'rules 16 and 19'; then
  echo "a --only run reached the clang-tidy gate" >&2
  exit 1
fi

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

"$ROOT/scripts/privacy-consent-test.sh" >/dev/null
"$ROOT/scripts/pki-test.sh" >/dev/null
"$ROOT/scripts/rpc-credentials-test.sh" >/dev/null

echo "build-all tests passed"
