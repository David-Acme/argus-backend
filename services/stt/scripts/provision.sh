#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

STT_RELEASE="asr-models"
STT_TARBALL="sherpa-onnx-nemo-fast-conformer-transducer-en-de-es-fr-14288-int8.tar.bz2"
STT_TARBALL_SHA256=""

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

setup_stt_model() {
  log "Setting up FastConformer Transducer STT model (en+de+es+fr, RTF ~0.02)..."
  log "License: CC-BY-4.0 (NVIDIA NeMo)"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/stt"
  local BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/$STT_RELEASE"

  if ! command -v curl >/dev/null 2>&1; then
    warn "curl not found; skipping STT model download."
    return
  fi

  if [ -f "$MODEL_DIR/nemo-transducer-encoder.int8.onnx" ]; then
    log "STT model already present."
    return
  fi

  mkdir -p "$MODEL_DIR"
  log "Fetching $STT_TARBALL (~107 MB)..."
  fetch_pinned "$BASE_URL/$STT_TARBALL" "$MODEL_DIR/$STT_TARBALL" "$STT_TARBALL_SHA256" || return 0

  local staging="$MODEL_DIR/.extract.part"
  rm -rf "$staging"
  mkdir -p "$staging"
  if ! tar -xjf "$MODEL_DIR/$STT_TARBALL" -C "$staging"; then
    rm -rf "$staging"
    warn "Extraction of $STT_TARBALL failed."
    return 0
  fi
  local found
  found="$(find "$staging" -name 'encoder.int8.onnx' -printf '%h' -quit)"
  local part
  for part in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
    if [ -z "$found" ] || [ ! -f "$found/$part" ]; then
      rm -rf "$staging"
      warn "$STT_TARBALL lacks $part; nothing installed."
      return 0
    fi
  done
  mv "$found/decoder.int8.onnx" "$MODEL_DIR/nemo-transducer-decoder.int8.onnx"
  mv "$found/joiner.int8.onnx" "$MODEL_DIR/nemo-transducer-joiner.int8.onnx"
  mv "$found/tokens.txt" "$MODEL_DIR/nemo-transducer-tokens.txt"
  mv "$found/encoder.int8.onnx" "$MODEL_DIR/nemo-transducer-encoder.int8.onnx"
  rm -rf "$staging" "$MODEL_DIR/$STT_TARBALL"

  log "STT model ready (~107 MB)."
}

setup_stt_model
