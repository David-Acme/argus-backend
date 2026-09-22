#!/usr/bin/env bash
# Section 2.4's tier table, enforced (plan section 4.13, rule 7): every tracked
# CMakeLists.txt under packages/ and services/ is read, and an edge the table
# forbids fails this script. The scanner is scripts/lib/check-deps.py; its
# --list-deferred prints the edges phase 3 inherits.
#
# usage: check-deps.sh [--list-deferred] [--root <dir>]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "check-deps: python3 is required" >&2
  exit 1
fi

exec python3 "$ROOT/scripts/lib/check-deps.py" --root "$ROOT" "$@"
