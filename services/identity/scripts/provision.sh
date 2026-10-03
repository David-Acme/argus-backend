#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_face_model() {
  log "Setting up Face Recognition models..."

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/face"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping Face model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  local BASE="https://raw.githubusercontent.com/Qengineering/Face-Recognition-Jetson-Nano/main/models"

  if [ ! -f "$MODEL_DIR/detector.param" ] || [ "$(wc -c < "$MODEL_DIR/detector.param")" -lt 100 ]; then
    log "Downloading RetinaFace detector..."
    $DL "$MODEL_DIR/detector.param" "$BASE/retina/mnet.25-opt.param" || warn "Failed: detector.param"
    $DL "$MODEL_DIR/detector.bin"   "$BASE/retina/mnet.25-opt.bin"   || warn "Failed: detector.bin"
  fi

  if [ ! -f "$MODEL_DIR/recognizer.param" ] || [ "$(wc -c < "$MODEL_DIR/recognizer.param")" -lt 100 ]; then
    log "Downloading MobileFaceNet recognizer..."
    $DL "$MODEL_DIR/recognizer.param" "$BASE/mobilefacenet/mobilefacenet.param" || warn "Failed: recognizer.param"
    $DL "$MODEL_DIR/recognizer.bin"   "$BASE/mobilefacenet/mobilefacenet.bin"   || warn "Failed: recognizer.bin"
  fi

  log "Face recognition models ready (~5 MB)."
}

setup_speaker_model() {
  log "Setting up the speaker-verification model (3D-Speaker ERes2Net, 26 MB)..."
  log "License: Apache License 2.0 (see models/speaker/NOTICE)"

  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/speaker"
  local BASE="https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models"
  local MODEL_FILE="3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"
  local MODEL_SHA256="c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5"
  local MODEL_PATH="$MODEL_DIR/$MODEL_FILE"
  local MODEL_TMP="$MODEL_PATH.part"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -fL --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping the speaker model download."
    return
  fi

  if ! command -v sha256sum >/dev/null 2>&1 &&
     ! command -v shasum >/dev/null 2>&1; then
    warn "Neither sha256sum nor shasum found; skipping the speaker model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ -f "$MODEL_PATH" ] && [ "$(sha256_file "$MODEL_PATH")" != "$MODEL_SHA256" ]; then
    warn "Existing speaker model checksum mismatch; replacing it."
    rm -f "$MODEL_PATH"
  fi

  if [ ! -f "$MODEL_PATH" ]; then
    rm -f "$MODEL_TMP"
    log "Downloading $MODEL_FILE..."
    if $DL "$MODEL_TMP" "$BASE/$MODEL_FILE"; then
      local ACTUAL
      ACTUAL="$(sha256_file "$MODEL_TMP")"
      if [ "$ACTUAL" = "$MODEL_SHA256" ]; then
        mv "$MODEL_TMP" "$MODEL_PATH"
        log "Speaker model checksum verified."
      else
        rm -f "$MODEL_TMP"
        warn "Checksum mismatch for $MODEL_FILE (expected $MODEL_SHA256, got $ACTUAL)."
      fi
    else
      rm -f "$MODEL_TMP"
      warn "Failed: $MODEL_FILE"
    fi
  else
    log "Speaker model already present and checksum verified."
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ] || ! grep -Fq "$MODEL_FILE" "$MODEL_DIR/NOTICE"; then
    cat > "$MODEL_DIR/NOTICE" <<'NOTICE_EOF'
ERes2Net speaker verification — 3D-Speaker (Alibaba Tongyi Lab)

Model:      ERes2Net, English, VoxCeleb2 dev (5994 speakers), 16 kHz
Source:     https://www.modelscope.cn/models/iic/speech_eres2net_sv_en_voxceleb_16k
Code:       https://github.com/modelscope/3D-Speaker
ONNX:       https://github.com/k2-fsa/sherpa-onnx/releases/tag/speaker-recongition-models
File:       3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx
SHA-256:    c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5
License:    Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0)
Paper:      Y. Chen et al., "An Enhanced Res2Net with Local and Global Feature
            Fusion for Speaker Verification", INTERSPEECH 2023.

Used by argus-identity to turn a confirmed voice enrollment into a 192-value
voiceprint. Raw audio is never stored; only the embedding centroid is.
NOTICE_EOF
  fi
}

setup_face_model
setup_speaker_model
