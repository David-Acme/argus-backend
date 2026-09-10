#!/usr/bin/env bash
# Model provisioning for argus-stt (moved verbatim from scripts/setup.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_stt_model() {
  log "Setting up FastConformer Transducer STT model (en+de+es+fr, RTF ~0.02)..."
  log "License: CC-BY-4.0 (NVIDIA NeMo)"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/stt"
  local BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
  local TARBALL="sherpa-onnx-nemo-fast-conformer-transducer-en-de-es-fr-14288-int8.tar.bz2"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping STT model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/nemo-transducer-encoder.int8.onnx" ]; then
    log "Downloading $TARBALL (~107 MB)..."
    $DL "$MODEL_DIR/$TARBALL" "$BASE_URL/$TARBALL" || {
      warn "Failed: $TARBALL"
      return
    }

    if [ -f "$MODEL_DIR/$TARBALL" ]; then
      log "Extracting $TARBALL..."
      mkdir -p "$MODEL_DIR/tmp"
      tar -xjf "$MODEL_DIR/$TARBALL" -C "$MODEL_DIR/tmp" \
        2>/dev/null || warn "Extraction may be incomplete."
      local TMPDIR
      TMPDIR="$(find "$MODEL_DIR/tmp" -name 'encoder.int8.onnx' -printf '%h' -quit)"
      if [ -z "$TMPDIR" ]; then
        TMPDIR="$MODEL_DIR/tmp"
      fi
      # Rename to the names SttService::createRecognizer() expects.
      if [ -f "$TMPDIR/encoder.int8.onnx" ]; then
        mv "$TMPDIR/encoder.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-encoder.int8.onnx"
      fi
      if [ -f "$TMPDIR/decoder.int8.onnx" ]; then
        mv "$TMPDIR/decoder.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-decoder.int8.onnx"
      fi
      if [ -f "$TMPDIR/joiner.int8.onnx" ]; then
        mv "$TMPDIR/joiner.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-joiner.int8.onnx"
      fi
      if [ -f "$TMPDIR/tokens.txt" ]; then
        mv "$TMPDIR/tokens.txt" \
           "$MODEL_DIR/nemo-transducer-tokens.txt"
      fi
      rm -rf "$MODEL_DIR/tmp" "$MODEL_DIR/$TARBALL"
    fi
  else
    log "STT model already present."
  fi

  log "STT model ready (~107 MB)."
}

setup_stt_model
