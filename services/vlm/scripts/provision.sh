#!/usr/bin/env bash
# Model provisioning for argus-vlm (moved verbatim from scripts/setup.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_vlm_gguf_model() {
  local VARIANT="${ARGUS_VLM_VARIANT:-lfm25}"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping GGUF VLM download."
    return
  fi

  local MODEL_DIR HF_BASE LM_FILE MMPROJ_FILE LICENSE_URL

  case "$VARIANT" in
    lfm2)
      MODEL_DIR="$ROOT/models/vision/lfm2vl"
      HF_BASE="https://huggingface.co/LiquidAI/LFM2-VL-450M-GGUF/resolve/main"
      LM_FILE="LFM2-VL-450M-Q8_0.gguf"
      MMPROJ_FILE="mmproj-LFM2-VL-450M-F16.gguf"
      LICENSE_URL="https://huggingface.co/LiquidAI/LFM2-VL-450M-GGUF/raw/main/LICENSE"
      log "Setting up LiquidAI LFM2-VL-450M (GGUF Q8_0 + mmproj F16, ~547 MB)..."
      ;;
    lfm25)
      MODEL_DIR="$ROOT/models/vision/lfm2vl-25"
      HF_BASE="https://huggingface.co/LiquidAI/LFM2.5-VL-450M-GGUF/resolve/main"
      LM_FILE="LFM2.5-VL-450M-Q8_0.gguf"
      MMPROJ_FILE="mmproj-LFM2.5-VL-450m-F16.gguf"
      LICENSE_URL="https://huggingface.co/LiquidAI/LFM2.5-VL-450M-GGUF/raw/main/LICENSE"
      log "Setting up LiquidAI LFM2.5-VL-450M (GGUF Q8_0 + mmproj F16, ~568 MB)..."
      ;;
    none)
      log "ARGUS_VLM_VARIANT=none, skipping GGUF VLM download."
      return
      ;;
    *)
      warn "Unknown ARGUS_VLM_VARIANT='$VARIANT' (use lfm2, lfm25 or none)."
      return
      ;;
  esac

  log "License: LFM Open License v1.0 (see LICENSE.lfm1.0 in $MODEL_DIR)"
  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/lm-Q8_0.gguf" ]; then
    log "Downloading $LM_FILE (~379 MB)..."
    $DL "$MODEL_DIR/lm-Q8_0.gguf" "$HF_BASE/$LM_FILE" || warn "Failed: $LM_FILE"
  else
    log "VLM language model already present."
  fi

  if [ ! -f "$MODEL_DIR/mmproj-F16.gguf" ]; then
    log "Downloading $MMPROJ_FILE (~190 MB)..."
    $DL "$MODEL_DIR/mmproj-F16.gguf" "$HF_BASE/$MMPROJ_FILE" \
      || warn "Failed: $MMPROJ_FILE"
  else
    log "VLM multimodal projector already present."
  fi

  if [ ! -f "$MODEL_DIR/LICENSE.lfm1.0" ]; then
    log "Downloading LFM Open License v1.0..."
    $DL "$MODEL_DIR/LICENSE.lfm1.0" "$LICENSE_URL" || warn "Failed: LICENSE.lfm1.0"
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ]; then
    cat > "$MODEL_DIR/NOTICE" <<NOTICE_EOF
${LM_FILE%.gguf} — Liquid AI

Model:      ${LM_FILE%.gguf} (Q8_0) + multimodal projector (F16)
Source:     $HF_BASE
License:    LFM Open License v1.0 — see LICENSE.lfm1.0 in this directory

Runs through llama.cpp + libmtmd, sharing the runtime already loaded for the
text LLM. Vision input is tiled at 256x256 with a scale factor of 2, giving
64 image tokens per tile; the number of tiles (and therefore the prompt cost)
grows with the input resolution, so callers downscale before captioning.

Key license terms that apply to this project:

  - Use, reproduction, modification and redistribution are permitted
    provided recipients receive a copy of the license and the
    attribution notices are retained.
  - Commercial use is conditioned on the licensee's annual revenue
    not exceeding USD \$10,000,000 (the "Threshold"). Qualified
    non-profit organizations are exempt from the Threshold for
    non-commercial or research purposes.
  - No warranty or liability is provided; the work is provided "AS IS".
  - This project does not claim any trademark rights in Liquid AI's
    marks, which are used only to describe the origin of the model.
NOTICE_EOF
  fi

  log "GGUF VLM ready in $MODEL_DIR"
}

setup_vlm_gguf_model
