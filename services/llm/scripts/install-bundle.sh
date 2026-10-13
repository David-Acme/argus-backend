#!/usr/bin/env bash
set -euo pipefail

LLM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$LLM_ROOT/../.." && pwd)"

usage() {
  cat <<'EOF'
usage: install-bundle.sh <decide|extract> <bundle-dir> [--slot DIR] [--config PATH]
       install-bundle.sh <decide|extract> --rollback [--slot DIR] [--config PATH]

Installs a trained model bundle into the slot argus-llm reads: the decider
(Laya) slot for decide, the extractor (GLiNER) slot for extract. The slot is
[decide] or [extract] bundle_dir in the service config (services/llm/config.toml
by default), or the shipped default when the config names none; --slot names it
directly.

The bundle is verified before anything moves: every file its sha256 file lists
matches, no file is unlisted, manifest.json names the model the slot serves
and records the hash of model.onnx (and, for a decider, of questions.json),
and a decider's labels all name a tool the server serves. It is copied beside
the slot, verified again, and swapped in; the bundle it replaces is kept as
<slot>.previous. --rollback swaps the two back. Restart argus-llm to load it.
EOF
}

fail() {
  printf 'install-bundle: %s\n' "$*" >&2
  exit 1
}

verify() {
  python3 -I - "$1" "$2" "$LLM_ROOT/tools/laya-labels.tsv" <<'PY'
import hashlib
import json
import pathlib
import sys

bundle, kind, labels_path = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
model = {"decide": "laya", "extract": "gliner"}[kind]
layout = ["model.onnx", "tokenizer", "labels.json", "decision.json", "max_len", "model-card.md", "sha256", "manifest.json"]
if kind == "decide":
    layout.append("questions.json")


def refuse(reason):
    print(f"{bundle}: {reason}", file=sys.stderr)
    sys.exit(1)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


if not bundle.is_dir():
    refuse("the bundle is not there")
for entry in layout:
    if not (bundle / entry).exists():
        refuse(f"the bundle is missing {entry}")
listed = {}
for line in (bundle / "sha256").read_text().splitlines():
    if not line:
        continue
    digest, separator, name = line.partition("  ")
    if not separator or len(digest) != 64 or not name:
        refuse("the sha256 file has a line that is not in sha256sum form")
    listed[name.strip()] = digest.lower()
present = {path.relative_to(bundle).as_posix() for path in bundle.rglob("*") if path.is_file()}
for name, digest in listed.items():
    if name not in present:
        refuse(f"sha256 names {name}, which is not in the bundle")
    if sha256_of(bundle / name) != digest:
        refuse(f"{name} does not match its line in sha256")
for name in sorted(present - set(listed) - {"sha256"}):
    refuse(f"the bundle holds {name}, which sha256 does not list")
manifest = json.loads((bundle / "manifest.json").read_text())
if manifest.get("model") != model:
    refuse(f"manifest.json names the model {manifest.get('model')!r}, and this slot serves {model!r}")
if (manifest.get("modelOnnx") or {}).get("sha256", "").lower() != listed.get("model.onnx"):
    refuse("manifest.json does not record the hash of the model.onnx this bundle carries")
if kind == "decide":
    questions = manifest.get("questions") or {}
    if questions.get("file") != "questions.json" or questions.get("sha256", "").lower() != listed.get("questions.json"):
        refuse("manifest.json does not record the hash of the questions.json this bundle carries")
    served = {line.split("\t")[0] for line in labels_path.read_text().splitlines() if "\t" in line}
    for label in json.loads((bundle / "labels.json").read_text()).get("labels", []):
        if label not in served and label not in ("none", "ask"):
            refuse(f"the bundle names the label {label!r}, which no tool serves")
fit = (manifest.get("calibration") or {}).get("fitOn", "")
print(f"verified {model} bundle {bundle.name}: model.onnx {listed['model.onnx'][:12]}, thresholds fit on {fit or 'an unrecorded split'}")
PY
}

configured_slot() {
  python3 -I - "$1" "$2" <<'PY'
import pathlib
import sys
import tomllib

config, kind = pathlib.Path(sys.argv[1]), sys.argv[2]
default = {"decide": "models/decide/bundle-laya-pilot", "extract": "models/extract/pilot-bundle"}[kind]
value = ""
if config.is_file():
    value = tomllib.loads(config.read_text()).get(kind, {}).get("bundle_dir", "")
print(value or default)
PY
}

kind=""
source_dir=""
slot=""
config="$LLM_ROOT/config.toml"
rollback=0
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --rollback) rollback=1; shift ;;
    --slot) [ $# -ge 2 ] || { usage >&2; exit 1; }; slot="$2"; shift 2 ;;
    --config) [ $# -ge 2 ] || { usage >&2; exit 1; }; config="$2"; shift 2 ;;
    decide | extract)
      [ -z "$kind" ] || fail "one slot per run"
      kind="$1"
      shift
      ;;
    -*) usage >&2; exit 1 ;;
    *) source_dir="$1"; shift ;;
  esac
done
[ -n "$kind" ] || { usage >&2; exit 1; }
if [ -z "$slot" ]; then
  slot="$(configured_slot "$config" "$kind")"
fi
case "$slot" in
  /*) ;;
  *) slot="$ROOT/$slot" ;;
esac
slot="${slot%/}"
previous="$slot.previous"
incoming="$slot.incoming"

if [ "$rollback" -eq 1 ]; then
  [ -d "$previous" ] || fail "no previous bundle to restore at $previous"
  verify "$previous" "$kind" >/dev/null || fail "the previous bundle at $previous no longer verifies"
  rm -rf "$incoming"
  if [ -d "$slot" ]; then
    mv "$slot" "$incoming"
  fi
  mv "$previous" "$slot"
  if [ -d "$incoming" ]; then
    mv "$incoming" "$previous"
  fi
  printf 'restored %s\n' "$slot"
  exit 0
fi

[ -n "$source_dir" ] || { usage >&2; exit 1; }
[ -d "$source_dir" ] || fail "the bundle $source_dir is not there"
source_dir="$(cd "$source_dir" && pwd)"
[ "$source_dir" != "$slot" ] || fail "the bundle is already the one in $slot"
verify "$source_dir" "$kind" >/dev/null || fail "$source_dir was refused"
mkdir -p "$(dirname "$slot")"
rm -rf "$incoming"
cp -a "$source_dir" "$incoming"
verify "$incoming" "$kind" || { rm -rf "$incoming"; fail "the copy at $incoming does not verify"; }
if [ -d "$slot" ]; then
  rm -rf "$previous"
  mv "$slot" "$previous"
fi
mv "$incoming" "$slot"
printf 'installed %s\n' "$slot"
