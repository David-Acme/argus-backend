#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "usage: $0 [--cloud DIR] [--into DIR]"
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLOUD="${ARGUS_CLOUD_VECTORS_DIR:-$ROOT/../../../argus-cloud/tests/protocol/vectors}"
INTO="$ROOT/tests/vectors"

while [ $# -gt 0 ]; do
  case "$1" in
    --cloud) [ $# -ge 2 ] || { usage; exit 1; }; CLOUD="$2"; shift ;;
    --into)  [ $# -ge 2 ] || { usage; exit 1; }; INTO="$2"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

[ -d "$CLOUD" ] || { echo "no vector directory at $CLOUD" >&2; exit 1; }

mkdir -p "$INTO"
for file in "$CLOUD"/*.json; do
  name="$(basename "$file")"
  [ "$name" = "pins.json" ] && continue
  cp "$file" "$INTO/$name"
done

count=0
{
  printf '{\n  "algorithm": "sha256",\n  "files": {'
  for file in "$INTO"/*.json; do
    name="$(basename "$file")"
    [ "$name" = "pins.json" ] && continue
    sum="$(sha256sum "$file" | cut -d' ' -f1)"
    [ "$count" -eq 0 ] || printf ','
    printf '\n    "%s": "%s"' "$name" "$sum"
    count=$((count + 1))
  done
  printf '\n  }\n}\n'
} > "$INTO/pins.json"

echo "$count vectors pinned in $INTO/pins.json"
