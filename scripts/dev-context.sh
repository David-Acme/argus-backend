#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Auto-setup venv if missing
if [ ! -f "$ROOT/.venv/bin/python3" ]; then
  echo "→ Creating .venv and installing tree-sitter ..." >&2
  python3 -m venv "$ROOT/.venv"
  "$ROOT/.venv/bin/pip" install --quiet tree-sitter tree-sitter-cpp
fi

OUTFILE="${1:-}"  # optional first arg = output file
if [ -n "$OUTFILE" ]; then
  "$ROOT/.venv/bin/python3" "$ROOT/scripts/dev-context.py" --output "$OUTFILE"
else
  "$ROOT/.venv/bin/python3" "$ROOT/scripts/dev-context.py"
fi
