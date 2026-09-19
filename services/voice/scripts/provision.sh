#!/usr/bin/env bash
# Model provisioning for argus-voice (moved verbatim from scripts/setup.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_vad_model() {
  log "Setting up Silero VAD model..."

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/vad"
  local BASE_URL="https://github.com/snakers4/silero-vad/raw/v5.0/files"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping VAD model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/silero_vad.onnx" ]; then
    log "Downloading silero_vad.onnx (~2.3 MB)..."
    $DL "$MODEL_DIR/silero_vad.onnx" "$BASE_URL/silero_vad.onnx" || \
      warn "Failed: silero_vad.onnx"
  else
    log "VAD model already present."
  fi

  log "VAD model ready (~2.3 MB)."
}

setup_vad_model
