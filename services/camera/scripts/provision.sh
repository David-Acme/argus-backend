#!/usr/bin/env bash
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

GO2RTC_VERSION="v1.9.14"

go2rtc_asset_sha256() {
  case "$1" in
    go2rtc_linux_amd64) echo "32d616af226bd731678ffde328b94cfb94e30339bfefc469cfb76323144615a6" ;;
    go2rtc_linux_arm64) echo "359fabade8a7a51e81a55fe6df6b0ef81764a5e1d63179577534eaaa71904b50" ;;
    go2rtc_linux_arm) echo "4d7e1639af5a2722a28e864468fd8099b3c1682565446c798bf9e3b38fde12e4" ;;
    go2rtc_mac_amd64.zip) echo "9b0b9a27a4dc3a5b8b93376e7e8fc2787c6af624a512842622be84aec0171c7a" ;;
    go2rtc_mac_arm64.zip) echo "919b78adc759d6b3883d1e1b2ac915ac0985bb903ff1897b4d228527bd64690c" ;;
    *) echo "" ;;
  esac
}

setup_go2rtc() {
  local ROOT
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
  local DEST="$ROOT/third_party/go2rtc"
  local BIN="$DEST/go2rtc"
  local WANT="${GO2RTC_VERSION#v}"

  if [ -x "$BIN" ] && "$BIN" -version 2>&1 | head -1 | grep -q "version $WANT "; then
    log "go2rtc $GO2RTC_VERSION already present"
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
  if [ "$os" = "mac" ]; then
    asset="$asset.zip"
  fi
  local SHA256
  SHA256="$(go2rtc_asset_sha256 "$asset")"
  if [ -z "$SHA256" ]; then
    warn "go2rtc: no pinned checksum for $asset; skipping."
    return
  fi

  local DL=""
  if command -v curl >/dev/null 2>&1; then
    DL="curl -fL --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping go2rtc download."
    return
  fi

  log "Downloading go2rtc $GO2RTC_VERSION ($asset)..."
  mkdir -p "$DEST"
  local PART="$DEST/$asset.part"
  rm -f "$PART"
  local URL="https://github.com/AlexxIT/go2rtc/releases/download/$GO2RTC_VERSION/$asset"
  if ! $DL "$PART" "$URL"; then
    rm -f "$PART"
    warn "Failed to download go2rtc; the camera pipeline will not start."
    return
  fi
  local ACTUAL
  ACTUAL="$(sha256_file "$PART")"
  if [ "$ACTUAL" != "$SHA256" ]; then
    rm -f "$PART"
    warn "Checksum mismatch for $asset (expected $SHA256, got $ACTUAL); go2rtc not installed."
    return
  fi
  if [ "$os" = "mac" ]; then
    if ! command -v unzip >/dev/null 2>&1; then
      rm -f "$PART"
      warn "unzip not found; cannot unpack $asset."
      return
    fi
    rm -rf "$DEST/.unpack"
    mkdir -p "$DEST/.unpack"
    unzip -q "$PART" -d "$DEST/.unpack"
    rm -f "$PART"
    if [ ! -f "$DEST/.unpack/go2rtc" ]; then
      rm -rf "$DEST/.unpack"
      warn "$asset did not contain go2rtc."
      return
    fi
    mv "$DEST/.unpack/go2rtc" "$PART"
    rm -rf "$DEST/.unpack"
  fi
  chmod +x "$PART"
  mv -f "$PART" "$BIN"
  log "go2rtc ready: $("$BIN" -version 2>&1 | head -1)"
}

setup_camera_model
setup_go2rtc
