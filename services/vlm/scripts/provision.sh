#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

LFM2_REVISION=""
LFM2_LM_SHA256=""
LFM2_MMPROJ_SHA256=""
LFM25_REVISION=""
LFM25_LM_SHA256=""
LFM25_MMPROJ_SHA256=""

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

pinned_revision() {
  local revision="$1"
  if [ -n "$revision" ]; then
    printf '%s' "$revision"
  elif [ "${ARGUS_ALLOW_UNPINNED_MODELS:-0}" = "1" ]; then
    printf 'main'
  fi
}


setup_vlm_gguf_model() {
  local VARIANT="${ARGUS_VLM_VARIANT:-lfm25}"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

  if ! command -v curl >/dev/null 2>&1; then
    warn "curl not found; skipping GGUF VLM download."
    return
  fi

  local MODEL_DIR HF_REPO REVISION LM_FILE MMPROJ_FILE LM_SHA256 MMPROJ_SHA256

  case "$VARIANT" in
    lfm2)
      MODEL_DIR="$ROOT/models/vision/lfm2vl"
      HF_REPO="https://huggingface.co/LiquidAI/LFM2-VL-450M-GGUF"
      REVISION="$(pinned_revision "$LFM2_REVISION")"
      LM_FILE="LFM2-VL-450M-Q8_0.gguf"
      MMPROJ_FILE="mmproj-LFM2-VL-450M-F16.gguf"
      LM_SHA256="$LFM2_LM_SHA256"
      MMPROJ_SHA256="$LFM2_MMPROJ_SHA256"
      log "Setting up LiquidAI LFM2-VL-450M (GGUF Q8_0 + mmproj F16, ~547 MB)..."
      ;;
    lfm25)
      MODEL_DIR="$ROOT/models/vision/lfm2vl-25"
      HF_REPO="https://huggingface.co/LiquidAI/LFM2.5-VL-450M-GGUF"
      REVISION="$(pinned_revision "$LFM25_REVISION")"
      LM_FILE="LFM2.5-VL-450M-Q8_0.gguf"
      MMPROJ_FILE="mmproj-LFM2.5-VL-450m-F16.gguf"
      LM_SHA256="$LFM25_LM_SHA256"
      MMPROJ_SHA256="$LFM25_MMPROJ_SHA256"
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

  if [ -z "$REVISION" ] && { [ ! -f "$MODEL_DIR/lm-Q8_0.gguf" ] || [ ! -f "$MODEL_DIR/mmproj-F16.gguf" ]; }; then
    warn "No revision pinned for $HF_REPO: pin it in $0, or run with ARGUS_ALLOW_UNPINNED_MODELS=1."
    return
  fi
  if [ -n "$REVISION" ]; then
    local HF_BASE="$HF_REPO/resolve/$REVISION"
    log "Fetching $LM_FILE (~379 MB)..."
    fetch_pinned "$HF_BASE/$LM_FILE" "$MODEL_DIR/lm-Q8_0.gguf" "$LM_SHA256" || true
    log "Fetching $MMPROJ_FILE (~190 MB)..."
    fetch_pinned "$HF_BASE/$MMPROJ_FILE" "$MODEL_DIR/mmproj-F16.gguf" "$MMPROJ_SHA256" || true
  else
    log "VLM model files already present (no revision pinned to verify them against)."
  fi

  if [ ! -f "$MODEL_DIR/LICENSE.lfm1.0" ] && [ -n "$REVISION" ]; then
    log "Downloading LFM Open License v1.0..."
    curl -fL --retry 3 --silent --show-error -o "$MODEL_DIR/LICENSE.lfm1.0.part" "$HF_REPO/raw/$REVISION/LICENSE" &&
      mv "$MODEL_DIR/LICENSE.lfm1.0.part" "$MODEL_DIR/LICENSE.lfm1.0" ||
      { rm -f "$MODEL_DIR/LICENSE.lfm1.0.part"; warn "Failed: LICENSE.lfm1.0"; }
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ]; then
    cat > "$MODEL_DIR/NOTICE" <<NOTICE_EOF
${LM_FILE%.gguf} — Liquid AI

Model:      ${LM_FILE%.gguf} (Q8_0) + multimodal projector (F16)
Source:     $HF_REPO (revision $REVISION)
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
