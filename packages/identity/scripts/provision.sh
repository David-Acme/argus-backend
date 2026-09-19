#!/usr/bin/env bash
# Model provisioning for argus-identity (moved verbatim from scripts/setup.sh).
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

  # RetinaFace (detector)
  if [ ! -f "$MODEL_DIR/detector.param" ] || [ "$(wc -c < "$MODEL_DIR/detector.param")" -lt 100 ]; then
    log "Downloading RetinaFace detector..."
    $DL "$MODEL_DIR/detector.param" "$BASE/retina/mnet.25-opt.param" || warn "Failed: detector.param"
    $DL "$MODEL_DIR/detector.bin"   "$BASE/retina/mnet.25-opt.bin"   || warn "Failed: detector.bin"
  fi

  # MobileFaceNet (recognizer)
  if [ ! -f "$MODEL_DIR/recognizer.param" ] || [ "$(wc -c < "$MODEL_DIR/recognizer.param")" -lt 100 ]; then
    log "Downloading MobileFaceNet recognizer..."
    $DL "$MODEL_DIR/recognizer.param" "$BASE/mobilefacenet/mobilefacenet.param" || warn "Failed: recognizer.param"
    $DL "$MODEL_DIR/recognizer.bin"   "$BASE/mobilefacenet/mobilefacenet.bin"   || warn "Failed: recognizer.bin"
  fi

  log "Face recognition models ready (~5 MB)."
}

setup_face_model
