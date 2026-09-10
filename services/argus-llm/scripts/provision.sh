#!/usr/bin/env bash
# Model provisioning for argus-llm (moved verbatim from scripts/setup.sh).
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

  # License compliance for the LFM Open License v1.0: ship the license text
  # and an attribution notice next to the model weights.
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

setup_llm_model
