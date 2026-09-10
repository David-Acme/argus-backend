#!/usr/bin/env bash
# Model provisioning for argus-tts (moved verbatim from scripts/setup.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_tts_model() {
  log "Setting up Supertonic 3 TTS model..."

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/tts"
  local ONNX_DIR="$MODEL_DIR/onnx"
  local VOICE_DIR="$MODEL_DIR/voice_styles"
  local HF_BASE="https://huggingface.co/Supertone/supertonic-3/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping TTS model download."
    return
  fi

  mkdir -p "$ONNX_DIR" "$VOICE_DIR"

  local ONNX_FILES=(
    duration_predictor.onnx
    text_encoder.onnx
    vector_estimator.onnx
    vocoder.onnx
    tts.json
    unicode_indexer.json
  )
  local VOICE_FILES=(
    F1.json F2.json F3.json F4.json F5.json
    M1.json M2.json M3.json M4.json M5.json
  )

  for f in "${ONNX_FILES[@]}"; do
    if [ ! -f "$ONNX_DIR/$f" ]; then
      log "Downloading onnx/$f..."
      $DL "$ONNX_DIR/$f" "$HF_BASE/onnx/$f" || warn "Failed: onnx/$f"
    fi
  done

  for f in "${VOICE_FILES[@]}"; do
    if [ ! -f "$VOICE_DIR/$f" ]; then
      log "Downloading voice_styles/$f..."
      $DL "$VOICE_DIR/$f" "$HF_BASE/voice_styles/$f" || warn "Failed: voice_styles/$f"
    fi
  done

  # License compliance for the Open RAIL-M license (Section 4): ship the
  # license text and an attribution/restrictions notice with the weights.
  if [ ! -f "$MODEL_DIR/LICENSE.openrail-m" ]; then
    log "Downloading Open RAIL-M license..."
    $DL "$MODEL_DIR/LICENSE.openrail-m" \
        "https://huggingface.co/Supertone/supertonic-3/raw/main/LICENSE" || \
      warn "Failed: LICENSE.openrail-m"
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ]; then
    cat > "$MODEL_DIR/NOTICE" <<'TTS_NOTICE_EOF'
Supertonic 3 — Supertone

Model:      Supertonic 3 (multilingual TTS, ~99M params, ONNX)
Source:     https://huggingface.co/Supertone/supertonic-3
Repo:       https://github.com/supertone-inc/supertonic
License:    BigScience Open RAIL-M License — see LICENSE.openrail-m

Use, modification, distribution and SaaS hosting are permitted provided
recipients receive a copy of the license and the use-based restrictions
(Attachment A) are carried into downstream agreements. No impersonation /
deepfakes, no law-enforcement / justice / immigration / asylum use, no
harmful false information. Output is owned by the user. Provided "AS IS".
TTS_NOTICE_EOF
  fi

  log "TTS model ready (~415 MB)."
}

setup_tts_model
