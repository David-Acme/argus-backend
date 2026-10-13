#!/usr/bin/env bash
set -euo pipefail

LLM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$LLM_ROOT/scripts/install-bundle.sh"
FIXTURES="$LLM_ROOT/tests/fixtures/bundles"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

fail() {
  printf 'install-bundle-test: %s\n' "$*" >&2
  exit 1
}

reseal() {
  local bundle="$1"
  (cd "$bundle" && find . -type f ! -name sha256 | sed 's|^\./||' | sort |
    while IFS= read -r name; do printf '%s  %s\n' "$(sha256sum "$name" | cut -d' ' -f1)" "$name"; done >sha256)
}

copy_of() {
  local target="$TEST_TMP/bundles/$1"
  rm -rf "$target"
  mkdir -p "$(dirname "$target")"
  cp -a "$FIXTURES/$2/pilot" "$target"
  printf '%s\n' "$target"
}

refused() {
  local kind="$1" bundle="$2" why="$3"
  if "$INSTALL" "$kind" "$bundle" --slot "$SLOT_DIR/$kind" --checker "$PASS" >/dev/null 2>&1; then
    fail "$why"
  fi
}

[ -d "$FIXTURES/decide/pilot" ] || fail "no decider fixture bundle at $FIXTURES/decide/pilot"
SLOT_DIR="$TEST_TMP/models"
PASS="$TEST_TMP/checker-pass"
FAILS="$TEST_TMP/checker-fail"
BARE="$TEST_TMP/checker-bare"
printf '#!/usr/bin/env bash\nexit 0\n' >"$PASS"
printf '#!/usr/bin/env bash\nexit 1\n' >"$FAILS"
printf '#!/usr/bin/env bash\nexit 3\n' >"$BARE"
chmod +x "$PASS" "$FAILS" "$BARE"

first="$(copy_of first decide)"
"$INSTALL" decide "$first" --slot "$SLOT_DIR/decide" --checker "$PASS" >/dev/null || fail "the fixture decider bundle was refused"
[ -f "$SLOT_DIR/decide/model.onnx" ] || fail "the bundle was not copied into the slot"
[ ! -e "$SLOT_DIR/decide.incoming" ] || fail "the staging copy was left behind"

second="$(copy_of second decide)"
printf '# the second card\n' >"$second/model-card.md"
reseal "$second"
"$INSTALL" decide "$second" --slot "$SLOT_DIR/decide" --checker "$PASS" >/dev/null || fail "a resealed bundle was refused"
grep -q 'the second card' "$SLOT_DIR/decide/model-card.md" || fail "the new bundle is not the one in the slot"
[ -d "$SLOT_DIR/decide.previous" ] || fail "the replaced bundle was not kept"
grep -q 'the second card' "$SLOT_DIR/decide.previous/model-card.md" && fail "the previous slot holds the new bundle"

"$INSTALL" decide --rollback --slot "$SLOT_DIR/decide" --checker "$PASS" >/dev/null || fail "the rollback failed"
grep -q 'the second card' "$SLOT_DIR/decide/model-card.md" && fail "the rollback did not restore the previous bundle"
grep -q 'the second card' "$SLOT_DIR/decide.previous/model-card.md" || fail "the rollback did not keep the bundle it replaced"

tampered="$(copy_of tampered decide)"
printf 'other weights' >"$tampered/model.onnx"
refused decide "$tampered" "a bundle whose model does not match its sha256 line was installed"

unlisted="$(copy_of unlisted decide)"
printf 'smuggled' >"$unlisted/extra.txt"
refused decide "$unlisted" "a bundle with an unlisted file was installed"

renamed="$(copy_of renamed decide)"
python3 -I - "$renamed/manifest.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
manifest = json.loads(path.read_text())
manifest["modelOnnx"]["sha256"] = "0" * 64
path.write_text(json.dumps(manifest, indent=1) + "\n")
PY
reseal "$renamed"
refused decide "$renamed" "a bundle whose manifest records another model hash was installed"

questions="$(copy_of questions decide)"
printf '{"tool": {"type": "choice", "instructions": "x", "criteria": ["camera"]}}\n' >"$questions/questions.json"
reseal "$questions"
refused decide "$questions" "a bundle whose manifest records another question set was installed"

stray="$(copy_of stray decide)"
printf '{"labels": ["memory_save", "holiday_planning"]}\n' >"$stray/labels.json"
reseal "$stray"
refused decide "$stray" "a bundle with a label no tool serves was installed"

refused extract "$first" "a decider bundle was installed in the extractor slot"

extractor="$(copy_of extractor extract)"
"$INSTALL" extract "$extractor" --slot "$SLOT_DIR/extract" --checker "$PASS" >/dev/null || fail "the fixture extractor bundle was refused"
[ -f "$SLOT_DIR/extract/model.onnx" ] || fail "the extractor bundle was not copied into its slot"

CONFIG="$TEST_TMP/config.toml"
printf '[decide]\nbundle_dir = "%s"\n' "$SLOT_DIR/configured" >"$CONFIG"
"$INSTALL" decide "$first" --config "$CONFIG" --checker "$PASS" >/dev/null || fail "the configured slot was not used"
[ -f "$SLOT_DIR/configured/model.onnx" ] || fail "the bundle did not land in the slot the config names"

golden="$(copy_of golden decide)"
if "$INSTALL" decide "$golden" --slot "$SLOT_DIR/parity" --checker "$FAILS" >/dev/null 2>&1; then
  fail "a bundle whose model misses its own golden vectors was installed"
fi
[ ! -e "$SLOT_DIR/parity" ] || fail "a refused bundle reached the slot"
[ ! -e "$SLOT_DIR/parity.incoming" ] || fail "a refused bundle left its staging copy"
"$INSTALL" decide "$golden" --slot "$SLOT_DIR/parity" --checker "$BARE" 2>"$TEST_TMP/bare.err" >/dev/null ||
  fail "a bundle without golden vectors was refused"
grep -q 'carries no golden vectors' "$TEST_TMP/bare.err" || fail "a bundle without golden vectors was installed silently"

untagged="$(copy_of untagged decide)"
python3 -I - "$untagged/manifest.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
manifest = json.loads(path.read_text())
manifest.pop("precision", None)
path.write_text(json.dumps(manifest, indent=1) + "\n")
PY
reseal "$untagged"
refused decide "$untagged" "a bundle that declares no precision was installed"

printf 'install-bundle-test: ok\n'
