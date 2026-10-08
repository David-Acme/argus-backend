#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

INSTALL="$ROOT/scripts/install-model-bundle.sh"

fail() {
  printf 'install-model-bundle-test: %s\n' "$*" >&2
  exit 1
}

resign() {
  local bundle="$1"
  (cd "$bundle" && find . -type f ! -name sha256 | sed 's|^\./||' | sort |
    while IFS= read -r name; do printf '%s  %s\n' "$(sha256sum "$name" | cut -d' ' -f1)" "$name"; done >sha256)
}

make_bundle() {
  local target="$1" source="$2"
  rm -rf "$target"
  mkdir -p "$(dirname "$target")"
  cp -a "$source" "$target"
}

PILOT="$ROOT/services/llm/tests/fixtures/bundles/decide/pilot"
[ -d "$PILOT" ] || fail "no pilot bundle at $PILOT"

MODELS="$TEST_TMP/models"
mkdir -p "$MODELS"
BUNDLES="$TEST_TMP/bundles"
mkdir -p "$BUNDLES"

CONFIG="$TEST_TMP/config.toml"
cat >"$CONFIG" <<'EOF'
[server]
port = 7032

[decide]
act = 0.90

[decide.laya]
existing = "kept"

[memory]
db_file = "database/memory.db"
EOF

make_bundle "$BUNDLES/one/pilot" "$PILOT"
"$INSTALL" laya "$BUNDLES/one/pilot" --config "$CONFIG" --models "$MODELS" >/dev/null ||
  fail "the pilot bundle was refused"

[ -d "$MODELS/decide/pilot" ] || fail "the bundle was not copied into models/decide/pilot"
grep -q "^bundle_dir = \"$MODELS/decide/pilot\"$" "$CONFIG" || fail "bundle_dir was not written"
grep -q '^sha256 = "' "$CONFIG" || fail "the pin was not written"
grep -q '^existing = "kept"$' "$CONFIG" || fail "an existing key in the table was lost"
grep -q '^act = 0.90$' "$CONFIG" || fail "a neighbouring table was lost"

make_bundle "$BUNDLES/two/pilot" "$PILOT"
printf '# the second card\n' >"$BUNDLES/two/pilot/model-card.md"
resign "$BUNDLES/two/pilot"
"$INSTALL" laya "$BUNDLES/two/pilot" --config "$CONFIG" --models "$MODELS" >/dev/null ||
  fail "the replacement bundle was refused"
grep -q 'the second card' "$MODELS/decide/pilot/model-card.md" || fail "the replacement was not installed"
[ -d "$MODELS/decide/pilot.previous" ] || fail "the previous bundle was not kept"

"$INSTALL" laya "$MODELS/decide/pilot" --rollback --config "$CONFIG" --models "$MODELS" >/dev/null ||
  fail "the rollback was refused"
grep -q 'pilot placeholder' "$MODELS/decide/pilot/model-card.md" || fail "the rollback did not restore the previous bundle"
grep -q "^bundle_dir = \"$MODELS/decide/pilot\"$" "$CONFIG" || fail "the rollback did not repoint the config"

make_bundle "$BUNDLES/badhash" "$PILOT"
printf 'tampered\n' >>"$BUNDLES/badhash/labels.json"
if "$INSTALL" laya "$BUNDLES/badhash" --config "$CONFIG" --models "$MODELS" >/dev/null 2>&1; then
  fail "a bundle whose file does not match its line was installed"
fi

make_bundle "$BUNDLES/vanished" "$PILOT"
rm "$BUNDLES/vanished/max_len"
if "$INSTALL" laya "$BUNDLES/vanished" --config "$CONFIG" --models "$MODELS" >/dev/null 2>&1; then
  fail "a bundle missing max_len was installed"
fi

make_bundle "$BUNDLES/straysha" "$PILOT"
printf 'extra\n' >"$BUNDLES/straysha/extra.txt"
if "$INSTALL" laya "$BUNDLES/straysha" --config "$CONFIG" --models "$MODELS" >/dev/null 2>&1; then
  fail "a bundle holding a file sha256 does not list was installed"
fi

make_bundle "$BUNDLES/straylabel" "$PILOT"
python3 -I -c '
import json, pathlib, sys
path = pathlib.Path(sys.argv[1], "labels.json")
labels = json.loads(path.read_text())["labels"]
labels["holiday_planning"] = "plan a holiday"
path.write_text(json.dumps({"labels": labels}, indent=1) + "\n")
' "$BUNDLES/straylabel"
resign "$BUNDLES/straylabel"
if "$INSTALL" laya "$BUNDLES/straylabel" --config "$CONFIG" --models "$MODELS" >/dev/null 2>&1; then
  fail "a bundle naming a label no tool serves was installed"
fi

printf 'install-model-bundle-test: ok\n'
