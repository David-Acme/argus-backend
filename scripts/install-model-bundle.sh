#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
usage: install-model-bundle.sh <laya|gliner> <bundle-dir> [--rollback]
                               [--config PATH] [--models DIR] [--labels PATH]

Verifies a model bundle against the bundle contract and the label set the
server's tools expect, keeps the previous bundle, and points the service at
the new one. --rollback restores the previous bundle. A restart of argus-llm
picks the change up.
EOF
}

fail() {
  printf 'install-model-bundle: %s\n' "$*" >&2
  exit 1
}

sha256_of() {
  sha256sum "$1" | cut -d' ' -f1
}

LAYOUT=(model.onnx tokenizer labels.json decision.json max_len model-card.md sha256 manifest.json)

check_layout() {
  local bundle="$1" entry
  for entry in "${LAYOUT[@]}"; do
    [ -e "$bundle/$entry" ] || fail "the bundle is missing $entry"
  done
}

check_digests() {
  local bundle="$1" line digest name
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    digest="${line%%  *}"
    name="${line#*  }"
    [ -f "$bundle/$name" ] || fail "sha256 names $name, which is not in the bundle"
    [ "$(sha256_of "$bundle/$name")" = "$digest" ] || fail "$name does not match its line in sha256"
  done <"$bundle/sha256"
  local path listed
  while IFS= read -r path; do
    name="${path#"$bundle"/}"
    [ "$name" = "sha256" ] && continue
    listed=0
    while IFS= read -r line; do
      [ "${line#*  }" = "$name" ] && listed=1
    done <"$bundle/sha256"
    [ "$listed" -eq 1 ] || fail "the bundle holds $name, which sha256 does not list"
  done < <(find "$bundle" -type f)
}

check_labels() {
  local bundle="$1" labels="$2" label
  while IFS= read -r label; do
    case "$label" in "" | none | ask) continue ;; esac
    awk -F'\t' -v wanted="$label" '$1 == wanted { found = 1 } END { exit found ? 0 : 1 }' "$labels" ||
      fail "the bundle names the label '$label', which no tool serves"
  done < <(python3 -I -c 'import json,sys;print("\n".join(json.load(open(sys.argv[1]))["labels"]))' "$bundle/labels.json")
}

verify_bundle() {
  local bundle="$1" labels="$2" check_labels_flag="$3"
  [ -d "$bundle" ] || fail "the bundle $bundle is not there"
  check_layout "$bundle"
  check_digests "$bundle"
  [ "$check_labels_flag" -eq 0 ] || check_labels "$bundle" "$labels"
}

write_config() {
  python3 -I - "$1" "$2" "$3" <<'PY'
import hashlib
import pathlib
import sys

path, section, bundle = sys.argv[1], sys.argv[2], sys.argv[3]
pin = hashlib.sha256(pathlib.Path(bundle, "sha256").read_bytes()).hexdigest()
wanted = [("bundle_dir", f'bundle_dir = "{bundle}"'), ("sha256", f'sha256 = "{pin}"')]
target = f"[{section}]"
lines = pathlib.Path(path).read_text().splitlines(keepends=True) if pathlib.Path(path).exists() else []
start = next((index for index, line in enumerate(lines) if line.strip() == target), None)
if start is None:
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    lines.append(target + "\n")
    start = len(lines) - 1
end = next((index for index in range(start + 1, len(lines)) if lines[index].lstrip().startswith("[")), len(lines))
seen = set()
for index in range(start + 1, end):
    for key, replacement in wanted:
        if lines[index].lstrip().startswith(key + " ="):
            lines[index] = replacement + "\n"
            seen.add(key)
missing = [replacement + "\n" for key, replacement in wanted if key not in seen]
if missing:
    if end < len(lines) and not lines[end - 1].endswith("\n"):
        lines[end - 1] += "\n"
    lines[end:end] = missing
pathlib.Path(path).write_text("".join(lines))
PY
}

model=""
bundle_dir=""
rollback=0
config="$ROOT/services/llm/config.toml"
models="$ROOT/models"
labels="$ROOT/services/llm/tools/laya-labels.tsv"
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --rollback) rollback=1; shift ;;
    --config) config="$2"; shift 2 ;;
    --models) models="$2"; shift 2 ;;
    --labels) labels="$2"; shift 2 ;;
    laya | gliner)
      [ -z "$model" ] || fail "one model per run"
      model="$1"
      shift
      ;;
    -*)
      usage >&2
      exit 1
      ;;
    *)
      bundle_dir="$1"
      shift
      ;;
  esac
done
[ -n "$model" ] || { usage >&2; exit 1; }

case "$model" in
  laya)
    target="decide"
    check_set=1
    ;;
  gliner)
    target="extract"
    check_set=0
    ;;
esac

[ -n "$bundle_dir" ] || { usage >&2; exit 1; }
bundle_dir="$(cd "$bundle_dir" && pwd)" || fail "the bundle $bundle_dir is not there"
name="$(basename "$bundle_dir")"
installed="$models/$target/$name"
previous="$models/$target/$name.previous"

if [ "$rollback" -eq 1 ]; then
  [ -d "$previous" ] || fail "no previous bundle to restore at $previous"
  rm -rf "$installed"
  mv "$previous" "$installed"
  write_config "$config" "$target.$model" "$installed"
  printf 'restored %s\n' "$installed"
  exit 0
fi

verify_bundle "$bundle_dir" "$labels" "$check_set"
mkdir -p "$models/$target"
if [ -d "$installed" ]; then
  rm -rf "$previous"
  mv "$installed" "$previous"
fi
cp -a "$bundle_dir" "$installed"
verify_bundle "$installed" "$labels" "$check_set"
write_config "$config" "$target.$model" "$installed"
python3 -I -c 'import json,sys;print("gate summary: " + json.dumps(json.load(open(sys.argv[1])).get("calibration", {})))' "$installed/manifest.json"
printf 'installed %s\n' "$installed"
