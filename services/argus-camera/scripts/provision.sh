#!/usr/bin/env bash
# Model provisioning for argus-camera (moved verbatim from scripts/setup.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

setup_camera_model() {
  log "Setting up YOLO26n object-detection model (AGPL-3.0, see argus-camera/NOTICE)..."
  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local MODEL_DIR="$ROOT/models/objects"
  local HF_BASE="https://huggingface.co/Ultralytics/YOLO26/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -fL --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping YOLO26n download."
  fi

  if ! command -v sha256sum >/dev/null 2>&1 &&
     ! command -v shasum >/dev/null 2>&1; then
    warn "Neither sha256sum nor shasum found; skipping YOLO26n download."
  fi

  mkdir -p "$MODEL_DIR"

  local PT_FILE="yolo26n.pt"
  local PT_SHA256="9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef"
  local PT_PATH="$MODEL_DIR/$PT_FILE"
  local PT_TMP="$PT_PATH.part"
  local PT_ACTUAL_SHA256=""

  if [ -n "$DL" ] &&
     { [ ! -f "$PT_PATH" ] ||
       [ "$(sha256_file "$PT_PATH" 2>/dev/null)" != "$PT_SHA256" ]; }; then
    rm -f "$PT_PATH"
    rm -f "$PT_TMP"
    log "Downloading $PT_FILE (~5.3 MB)..."
    if $DL "$PT_TMP" "$HF_BASE/$PT_FILE"; then
      PT_ACTUAL_SHA256="$(sha256_file "$PT_TMP")"
      if [ "$PT_ACTUAL_SHA256" = "$PT_SHA256" ]; then
        mv "$PT_TMP" "$PT_PATH"
        log "YOLO26n checksum verified."
      else
        rm -f "$PT_TMP"
        warn "Checksum mismatch for $PT_FILE (expected $PT_SHA256, got $PT_ACTUAL_SHA256)."
      fi
    else
      rm -f "$PT_TMP"
      warn "Failed: $PT_FILE"
    fi
  elif [ -f "$PT_PATH" ]; then
    log "YOLO26n weights already present and checksum verified."
  fi

  if [ -f "$MODEL_DIR/yolo26n.param" ] && [ -f "$MODEL_DIR/yolo26n.bin" ]; then
    log "NCNN detector artifacts already present."
    return
  fi

  if [ ! -f "$PT_PATH" ]; then
    warn "yolo26n.pt unavailable; the detector starts disabled until the model exists."
    return
  fi

  # The raw end-to-end NCNN artifacts are exported locally, never downloaded:
  # the one2one head keeps the raw XYXY output the C++ postprocess decodes.
  local PY=""
  if command -v python3 >/dev/null 2>&1; then
    PY="python3"
  elif command -v python >/dev/null 2>&1; then
    PY="python"
  fi
  if [ -z "$PY" ]; then
    warn "No python3/python found; export the NCNN artifacts manually:"
    warn "  pip install ultralytics torch pnnx"
    warn "  python3 scripts/export-yolo26-ncnn-e2e.py --weights models/objects/yolo26n.pt --imgsz 640"
    warn "  mv yolo26n_ncnn_e2e_raw_model/model.ncnn.param models/objects/yolo26n.param"
    warn "  mv yolo26n_ncnn_e2e_raw_model/model.ncnn.bin models/objects/yolo26n.bin"
    return
  fi
  if ! $PY -c "import ultralytics, torch, pnnx" >/dev/null 2>&1; then
    warn "python lacks ultralytics+torch+pnnx; export the NCNN artifacts manually:"
    warn "  pip install ultralytics torch pnnx"
    warn "  python3 scripts/export-yolo26-ncnn-e2e.py --weights models/objects/yolo26n.pt --imgsz 640"
    warn "  mv yolo26n_ncnn_e2e_raw_model/model.ncnn.param models/objects/yolo26n.param"
    warn "  mv yolo26n_ncnn_e2e_raw_model/model.ncnn.bin models/objects/yolo26n.bin"
    return
  fi

  log "Exporting raw end-to-end NCNN artifacts (one2one head, XYXY)..."
  rm -rf "$MODEL_DIR/.export-tmp"
  if $PY "$ROOT/scripts/export-yolo26-ncnn-e2e.py" \
      --weights "$PT_PATH" --imgsz 640 --out-dir "$MODEL_DIR/.export-tmp"; then
    mv "$MODEL_DIR/.export-tmp/model.ncnn.param" "$MODEL_DIR/yolo26n.param"
    mv "$MODEL_DIR/.export-tmp/model.ncnn.bin" "$MODEL_DIR/yolo26n.bin"
    rm -rf "$MODEL_DIR/.export-tmp"
    log "Detector model ready: $MODEL_DIR/yolo26n.param + yolo26n.bin"
  else
    rm -rf "$MODEL_DIR/.export-tmp"
    warn "Export failed; the detector starts disabled. Retry with:"
    warn "  python3 scripts/export-yolo26-ncnn-e2e.py --weights models/objects/yolo26n.pt --imgsz 640"
  fi
}

setup_go2rtc() {
  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local DEST="$ROOT/third_party/go2rtc"
  local BIN="$DEST/go2rtc"

  if [ -x "$BIN" ]; then
    log "go2rtc already present ($("$BIN" -version 2>&1 | head -1))"
    return
  fi

  local os arch asset
  case "$(uname -s)" in
    Linux)  os="linux" ;;
    Darwin) os="mac" ;;
    *) warn "go2rtc: unsupported OS $(uname -s), skipping."; return ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch="amd64" ;;
    aarch64|arm64) arch="arm64" ;;
    armv7l) arch="arm" ;;
    *) warn "go2rtc: unsupported arch $(uname -m), skipping."; return ;;
  esac
  asset="go2rtc_${os}_${arch}"

  local DL=""
  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping go2rtc download."
    return
  fi

  log "Downloading go2rtc ($asset)..."
  mkdir -p "$DEST"
  local URL="https://github.com/AlexxIT/go2rtc/releases/latest/download/$asset"
  if $DL "$BIN" "$URL"; then
    chmod +x "$BIN"
    log "go2rtc ready: $("$BIN" -version 2>&1 | head -1)"
  else
    warn "Failed to download go2rtc; the camera pipeline will not start."
    rm -f "$BIN"
  fi
}

setup_camera_model
setup_go2rtc
