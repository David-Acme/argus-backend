#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "check-tidy: python3 is required" >&2
  exit 1
fi

exec python3 "$ROOT/scripts/lib/tidy_scan.py" --root "$ROOT" "$@"
