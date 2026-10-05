#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

SILERO_REVISION="v5.0"
SILERO_SHA256=""

fetch_pinned() {
  local url="$1" target="$2" expected="$3"
  if [ -f "$target" ]; then
    if [ -z "$expected" ] || [ "$(sha256_file "$target")" = "$expected" ]; then
      return 0
    fi
    warn "Checksum mismatch for the present $(basename "$target"); replacing it."
    rm -f "$target"
  fi
  if [ -z "$expected" ] && [ "${ARGUS_ALLOW_UNPINNED_MODELS:-0}" != "1" ]; then
    warn "No SHA-256 pinned for $(basename "$target"): pin it in $0, or run with ARGUS_ALLOW_UNPINNED_MODELS=1 to fetch it and print its hash."
    return 1
  fi
  rm -f "$target.part"
  mkdir -p "$(dirname "$target")"
  if ! curl -fL --retry 3 --progress-bar -o "$target.part" "$url"; then
    rm -f "$target.part"
    warn "Download failed: $(basename "$target")"
    return 1
  fi
  local actual
  actual="$(sha256_file "$target.part")"
  if [ -n "$expected" ] && [ "$actual" != "$expected" ]; then
    rm -f "$target.part"
    warn "Checksum mismatch for $(basename "$target") (expected $expected, got $actual)."
    return 1
  fi
  if [ -z "$expected" ]; then
    warn "Unpinned download of $(basename "$target"): sha256 $actual. Pin it before the next release."
  fi
  mv "$target.part" "$target"
}

setup_vad_model() {
  log "Setting up Silero VAD model..."

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/vad"
  local BASE_URL="https://github.com/snakers4/silero-vad/raw/$SILERO_REVISION/files"

  if ! command -v curl >/dev/null 2>&1; then
    warn "curl not found; skipping VAD model download."
    return
  fi

  log "Fetching silero_vad.onnx (~2.3 MB, $SILERO_REVISION)..."
  if fetch_pinned "$BASE_URL/silero_vad.onnx" "$MODEL_DIR/silero_vad.onnx" "$SILERO_SHA256"; then
    log "VAD model ready (~2.3 MB)."
  fi
}

setup_vad_model
