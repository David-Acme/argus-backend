#!/usr/bin/env bash
# Rules 16 and 19, measured (plan section 4.13): scripts/lib/tidy_scan.py runs
# the .clang-tidy check set over every first-party translation unit and fails
# when a check's count rises above scripts/lib/tidy-baseline.txt. The tree
# carries thousands of findings today, so the gate is a ratchet -- rule 19 makes
# modernising what a change touches part of the change, and the baseline comes
# down as the old ones are fixed.
#
# usage: check-tidy.sh [--write-baseline] [--top <n>] [--root <dir>]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "check-tidy: python3 is required" >&2
  exit 1
fi

exec python3 "$ROOT/scripts/lib/tidy_scan.py" --root "$ROOT" "$@"
