#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_llm_model() {
  log "Setting up LiquidAI LFM2.5-1.2B-Instruct QAD model (no thinking, 696 MB)..."
  log "License: LFM Open License v1.0 (see models/llm/LICENSE.lfm1.0)"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/llm"
  local HF_BASE="https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -fL --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping LLM model download."
    return
  fi

  if ! command -v sha256sum >/dev/null 2>&1 &&
     ! command -v shasum >/dev/null 2>&1; then
    warn "Neither sha256sum nor shasum found; skipping LLM model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  local MODEL_FILE="LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf"
  local MODEL_SHA256="bb741ebb106d543e9de114b843a3d3d73d51c74b5801e69da2abde821a0cb3e1"
  local MODEL_PATH="$MODEL_DIR/$MODEL_FILE"
  local MODEL_TMP="$MODEL_PATH.part"
  local MODEL_ACTUAL_SHA256=""

  if [ -f "$MODEL_PATH" ]; then
    MODEL_ACTUAL_SHA256="$(sha256_file "$MODEL_PATH")"
    if [ "$MODEL_ACTUAL_SHA256" = "$MODEL_SHA256" ]; then
      log "LLM model already present and checksum verified."
    else
      warn "Existing LLM model checksum mismatch; replacing it."
      rm -f "$MODEL_PATH"
    fi
  fi

  if [ ! -f "$MODEL_PATH" ]; then
    rm -f "$MODEL_TMP"
    log "Downloading $MODEL_FILE (~696 MB)..."
    if $DL "$MODEL_TMP" "$HF_BASE/$MODEL_FILE"; then
      MODEL_ACTUAL_SHA256="$(sha256_file "$MODEL_TMP")"
      if [ "$MODEL_ACTUAL_SHA256" = "$MODEL_SHA256" ]; then
        mv "$MODEL_TMP" "$MODEL_PATH"
        log "LLM model checksum verified."
      else
        rm -f "$MODEL_TMP"
        warn "Checksum mismatch for $MODEL_FILE (expected $MODEL_SHA256, got $MODEL_ACTUAL_SHA256)."
      fi
    else
      rm -f "$MODEL_TMP"
      warn "Failed: $MODEL_FILE"
    fi
  fi

  local LICENSE_URL="https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/raw/main/LICENSE"
  if [ ! -f "$MODEL_DIR/LICENSE.lfm1.0" ]; then
    log "Downloading LFM Open License v1.0..."
    $DL "$MODEL_DIR/LICENSE.lfm1.0" "$LICENSE_URL" || warn "Failed: LICENSE.lfm1.0"
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ] ||
     ! grep -Fq "LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf" "$MODEL_DIR/NOTICE"; then
    cat > "$MODEL_DIR/NOTICE" <<'NOTICE_EOF'
LFM2.5-1.2B-Instruct QAD — Liquid AI

Model:      LFM2.5-1.2B-Instruct (QAD Q4_0)
Source:     https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct
GGUF:       https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF
File:       LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
SHA-256:    bb741ebb106d543e9de114b843a3d3d73d51c74b5801e69da2abde821a0cb3e1
License:    LFM Open License v1.0 — see LICENSE.lfm1.0 in this directory

This project uses the LFM2.5-1.2B-Instruct model distributed under the
LFM Open License v1.0 (Copyright (c) Liquid AI, Inc.).

Key license terms that apply to this project:

  - Use, reproduction, modification and redistribution are permitted
    provided recipients receive a copy of the license and the
    attribution notices are retained.
  - Commercial use is conditioned on the licensee's annual revenue
    not exceeding USD $10,000,000 (the "Threshold"). Qualified
    non-profit organizations are exempt from the Threshold for
    non-commercial or research purposes.
  - No warranty or liability is provided; the work is provided "AS IS".
  - This project does not claim any trademark rights in Liquid AI's
    marks, which are used only to describe the origin of the model.
NOTICE_EOF
  fi

  if [ -f "$MODEL_PATH" ]; then
    log "LLM model ready."
  else
    warn "LLM model is not available; the backend will fail to load its configured model."
  fi
}

setup_memory_model() {
  log "Setting up multilingual-e5-small embedding model (int8, 118 MB)..."
  log "License: MIT (see models/memory/LICENSE)"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/memory"
  local HF_BASE="https://huggingface.co/intfloat/multilingual-e5-small/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping memory embedding model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/model.onnx" ]; then
    log "Downloading model.onnx (~118 MB)..."
    $DL "$MODEL_DIR/model.onnx" \
        "$HF_BASE/onnx/model_qint8_avx512_vnni.onnx" || \
      warn "Failed: model.onnx"
  fi

  if [ ! -f "$MODEL_DIR/tokenizer.json" ]; then
    log "Downloading tokenizer.json (~17 MB)..."
    $DL "$MODEL_DIR/tokenizer.json" "$HF_BASE/onnx/tokenizer.json" || \
      warn "Failed: tokenizer.json"
  fi

  if [ ! -f "$MODEL_DIR/config.json" ]; then
    log "Downloading config.json..."
    $DL "$MODEL_DIR/config.json" "$HF_BASE/config.json" || \
      warn "Failed: config.json"
  fi

  if [ ! -f "$MODEL_DIR/LICENSE" ]; then
    log "Downloading MIT license..."
    $DL "$MODEL_DIR/LICENSE" "$HF_BASE/LICENSE" || warn "Failed: LICENSE"
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ]; then
    cat > "$MODEL_DIR/NOTICE" <<'MEMORY_NOTICE_EOF'
multilingual-e5-small — IntFloat / Microsoft

Model:      multilingual-e5-small (118M params, 384-dim embeddings, 100+ languages)
Source:     https://huggingface.co/intfloat/multilingual-e5-small
License:    MIT — see LICENSE in this directory

Used by the Argus MemoryService for semantic recall (hybrid BM25 + vector).
INT8 quantized ONNX export (model_qint8_avx512_vnni.onnx), runs on any x86-64
CPU through ONNX Runtime (no AVX-512 required). Provided "AS IS".
MEMORY_NOTICE_EOF
  fi

  log "Memory embedding model ready."
}

setup_turn_bundle() {
  local kind="$1" name="$2" variable="$3"
  local source="${!variable:-}"
  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local INSTALL="$ROOT/services/llm/scripts/install-bundle.sh"
  local SLOT="$ROOT/models/$kind/$name"

  if [ -d "$SLOT" ]; then
    log "The $kind bundle is present at models/$kind/$name."
    return 0
  fi
  if [ -n "$source" ]; then
    log "Installing the $kind bundle from $source..."
    local parity=()
    if [ ! -x "$ROOT/services/llm/build/prod/llm-bundle-check" ] && [ ! -x "$ROOT/services/llm/build/dev/llm-bundle-check" ]; then
      warn "argus-llm is not built yet, so the bundle's golden vectors are checked by its test suite, not now."
      parity=(--skip-parity)
    fi
    "$INSTALL" "$kind" "$source" --slot "$SLOT" "${parity[@]}" || warn "The $kind bundle at $source was refused."
    return 0
  fi
  warn "No $kind bundle at models/$kind/$name. The pilot bundles are exported by the intent-training"
  warn "project (scripts/kit_bundle.py) and are not downloadable; copy one in with"
  warn "  services/llm/scripts/install-bundle.sh $kind <bundle-dir>"
  warn "or set $variable to the bundle directory and run this script again."
  warn "Until then the turn runs on the rule tier alone."
}

setup_llm_model
setup_memory_model
setup_turn_bundle decide bundle-laya-mvp ARGUS_DECIDE_BUNDLE_SOURCE
setup_turn_bundle extract bundle-gliner-pilot ARGUS_EXTRACT_BUNDLE_SOURCE
