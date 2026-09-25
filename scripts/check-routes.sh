#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "check-routes: python3 is required" >&2
  exit 1
fi

exec python3 "$ROOT/scripts/lib/route_scan.py" --root "$ROOT" "$@"
