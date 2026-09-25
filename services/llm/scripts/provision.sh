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

setup_extract_model() {
  log "Setting up NuExtract-1.5-tiny structured-extraction model (Q4_K_M, 469 MB)..."
  log "License: MIT (see models/extract/LICENSE)"
  log "Optional: skipping it degrades memory extraction to the lexicon tier."

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/extract"
  local BASE_URL="https://huggingface.co/numind/NuExtract-1.5-tiny/resolve/main"
  local MODEL_FILE="NuExtract-1.5-tiny-Q4_K_M.gguf"
  local EXPECTED_BYTES=491400416
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --http1.1 --retry 5 --retry-delay 2 -C - --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --continue --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping extraction model download."
    return 0
  fi

  if [ ! -d "$MODEL_DIR" ]; then
    mkdir -p "$MODEL_DIR" || return 0
  fi

  local TARGET="$MODEL_DIR/$MODEL_FILE"
  if [ -f "$TARGET" ]; then
    local HAVE
    HAVE="$(wc -c < "$TARGET" | tr -d ' ')"
    if [ "$HAVE" = "$EXPECTED_BYTES" ]; then
      log "Extraction model already present, skipping download."
      return 0
    fi
    warn "Extraction model is truncated ($HAVE bytes), resuming."
  fi

  $DL "$TARGET" "$BASE_URL/$MODEL_FILE" || {
    warn "Extraction model download failed; continuing without it."
    return 0
  }

  local GOT
  GOT="$(wc -c < "$TARGET" | tr -d ' ')"
  if [ "$GOT" != "$EXPECTED_BYTES" ]; then
    warn "Extraction model size mismatch (got $GOT, expected $EXPECTED_BYTES)."
    warn "Re-run scripts/setup.sh to resume the download."
    return 0
  fi

  cat > "$MODEL_DIR/LICENSE" << 'MIT_EOF'
MIT License
Copyright (c) 2024 numind
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
MIT_EOF
  log "Extraction model ready."
}

setup_llm_model
setup_memory_model
setup_extract_model
