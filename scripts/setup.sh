#!/usr/bin/env bash
#
# Argus backend - setup script (Linux).
#
# Automates build with CMake presets. Unifies dev & prod profiles:
#   1. Install system build dependencies (distro aware)
#   2. Install Conan (if missing) and configure the profile for C++20
#   3. Download Supertonic 3 TTS model (~415 MB)
#   4. Install Conan dependencies into build/<profile>
#   5. Configure and build with the matching CMake preset
#
# Usage:
#   ./scripts/setup.sh                     # default: dev
#   ./scripts/setup.sh dev                 # dev profile
#   ./scripts/setup.sh prod                # prod profile
#   ./scripts/setup.sh dev --no-build      # install deps only (no compile)
#   SKIP_BUILD=1 ./scripts/setup.sh prod
#
set -euo pipefail

log()  { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    err "required command not found: $1"
    exit 1
  fi
}

sudo_if_needed() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    command -v sudo >/dev/null 2>&1 && sudo "$@" || "$@"
  fi
}

PROFILE="dev"
SKIP_BUILD=0
ARGS=()

for a in "$@"; do
  case "$a" in
    -h|--help)
      grep '^#' "$0" | sed 's/^#\{1,2\} //'; exit 0 ;;
    --no-build) SKIP_BUILD=1 ;;
    dev|prod)   PROFILE="$a" ;;
    *)          ARGS+=("$a") ;;
  esac
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug";   OUTPUT_FOLDER="build/dev";  CMAKE_PRESET="dev" ;;
  prod) BUILD_TYPE="Release"; OUTPUT_FOLDER="build/prod"; CMAKE_PRESET="prod" ;;
esac

log "Profile: $PROFILE  build_type: $BUILD_TYPE  output: $OUTPUT_FOLDER  preset: $CMAKE_PRESET"

install_system_deps() {
  log "Detecting distribution and installing build dependencies..."

  local missing=0
  for c in git cmake ninja g++ python3; do
    command -v "$c" >/dev/null 2>&1 || missing=1
  done
  if [ "$missing" -eq 0 ]; then
    log "Build dependencies already installed, skipping system package install."
    return
  fi

  local pkg_mgr=""
  if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "${ID:-},${ID_LIKE:-}" in
      *arch*|*manjaro*) pkg_mgr="pacman" ;;
      *debian*|*ubuntu*) pkg_mgr="apt" ;;
      *fedora*|*rhel*) pkg_mgr="dnf" ;;
      *alpine*) pkg_mgr="apk" ;;
      *opensuse*) pkg_mgr="zypper" ;;
    esac
  fi

  case "$pkg_mgr" in
    pacman)
      sudo_if_needed pacman -Syu --needed --noconfirm git cmake ninja gcc python python-pip base-devel ;;
    apt)
      sudo_if_needed apt-get update -y
      sudo_if_needed apt-get install -y --no-install-recommends git cmake ninja-build g++ python3 python3-pip pkg-config ca-certificates ;;
    dnf)
      sudo_if_needed dnf install -y git cmake ninja-build gcc-c++ python3 python3-pip ;;
    apk)
      sudo_if_needed apk add --no-cache git cmake ninja g++ python3 py3-pip build-base linux-headers ;;
    zypper)
      sudo_if_needed zypper install -y git cmake ninja gcc-c++ python3 python3-pip ;;
    *)
      warn "Unknown distribution. Ensure git cmake ninja g++ python3 pip are installed." ;;
  esac
}

ensure_conan() {
  if command -v conan >/dev/null 2>&1; then
    log "Conan already installed: $(conan --version)"
    return
  fi
  log "Conan not found, installing via pip..."
  local PIP=""
  command -v pip3 >/dev/null 2>&1 && PIP=pip3
  command -v pip  >/dev/null 2>&1 && PIP=pip
  [ -z "$PIP" ] && PIP="python3 -m pip"
  $PIP install --user conan 2>/dev/null || sudo_if_needed $PIP install conan 2>/dev/null || $PIP install --user --break-system-packages conan

  local pybin
  pybin="$(python3 -m site --user-base 2>/dev/null)/bin"
  case ":$PATH:" in
    *":$pybin:"*) ;;
    *) export PATH="$pybin:$PATH" ;;
  esac
}

configure_conan_profile() {
  log "Configuring Conan profile (default)..."
  if [ ! -f "$HOME/.conan2/profiles/default" ]; then
    conan profile detect
  fi
  local profile="$HOME/.conan2/profiles/default"
  if grep -q '^compiler.cppstd=' "$profile"; then
    sed -i 's/^compiler.cppstd=.*/compiler.cppstd=gnu20/' "$profile"
  else
    sed -i '/^compiler.version=/a compiler.cppstd=gnu20' "$profile"
  fi
  log "Conan profile ready: $profile"
}

setup_submodules() {
  log "Initialising git submodules (third_party/*)..."
  if [ ! -d ".git" ] && [ -f ".gitmodules" ]; then
    warn "Not a git worktree; skipping submodule init."
    return
  fi
  git submodule update --init --recursive 2>&1 | sed 's/^/  /' || \
    warn "Top-level submodule init failed (network?)."

  for d in third_party/*/; do
    if [ -d "${d}.git" ] || git -C "$d" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      log "Initialising nested submodules in $d"
      git -C "$d" submodule update --init --recursive 2>&1 | sed 's/^/  /' || \
        warn "Nested submodule init failed in $d (network?)."
    fi
  done

  if [ -f third_party/fastText/CMakeLists.txt ]; then
    sed -i 's/^set(CMAKE_CXX_STANDARD 17)/set(CMAKE_CXX_STANDARD 20)/' \
      third_party/fastText/CMakeLists.txt
  fi
}

setup_tts_model() {
  log "Setting up Supertonic 3 TTS model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/tts"
  local ONNX_DIR="$MODEL_DIR/onnx"
  local VOICE_DIR="$MODEL_DIR/voice_styles"
  local TTS_DIR="$ROOT/src/shared/tts"
  local HF_BASE="https://huggingface.co/Supertone/supertonic-3/resolve/main"
  local GH_BASE="https://raw.githubusercontent.com/supertone-inc/supertonic/main/cpp"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping TTS model download."
    return
  fi

  mkdir -p "$ONNX_DIR" "$VOICE_DIR" "$TTS_DIR"

  local ONNX_FILES=(
    duration_predictor.onnx
    text_encoder.onnx
    vector_estimator.onnx
    vocoder.onnx
    tts.json
    unicode_indexer.json
  )
  local VOICE_FILES=(
    F1.json F2.json F3.json F4.json F5.json
    M1.json M2.json M3.json M4.json M5.json
  )

  for f in "${ONNX_FILES[@]}"; do
    if [ ! -f "$ONNX_DIR/$f" ]; then
      log "Downloading onnx/$f..."
      $DL "$ONNX_DIR/$f" "$HF_BASE/onnx/$f" || warn "Failed: onnx/$f"
    fi
  done

  for f in "${VOICE_FILES[@]}"; do
    if [ ! -f "$VOICE_DIR/$f" ]; then
      log "Downloading voice_styles/$f..."
      $DL "$VOICE_DIR/$f" "$HF_BASE/voice_styles/$f" || warn "Failed: voice_styles/$f"
    fi
  done

  for f in helper.h helper.cpp; do
    if [ ! -f "$TTS_DIR/$f" ]; then
      log "Downloading $f from supertonic GitHub..."
      $DL "$TTS_DIR/$f" "$GH_BASE/$f" || warn "Failed: $f"
    fi
  done

  log "TTS model ready (~415 MB)."
}

build_project() {
  if [ "$SKIP_BUILD" -eq 1 ]; then
    log "Installing Conan dependencies into $OUTPUT_FOLDER ($BUILD_TYPE)..."
    conan install . --output-folder="$OUTPUT_FOLDER" -s "build_type=$BUILD_TYPE" --build=missing
    rm -f CMakeUserPresets.json
    log "Skipping build (--no-build / SKIP_BUILD). Done."
    return
  fi

  log "Installing Conan dependencies into $OUTPUT_FOLDER ($BUILD_TYPE)..."
  conan install . --output-folder="$OUTPUT_FOLDER" -s "build_type=$BUILD_TYPE" --build=missing
  rm -f CMakeUserPresets.json

  log "Configuring with CMake preset '$CMAKE_PRESET'..."
  cmake --preset "$CMAKE_PRESET"

  log "Building with CMake preset '$CMAKE_PRESET'..."
  cmake --build --preset "$CMAKE_PRESET"

  log "Build complete. Run the server from: $OUTPUT_FOLDER/argus-backend"
}

setup_llm_model() {
  log "Setting up LiquidAI LFM2.5-1.2B-Instruct model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/llm"
  local HF_BASE="https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping LLM model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  local MODEL_FILE="LFM2.5-1.2B-Instruct-Q4_K_M.gguf"

  if [ ! -f "$MODEL_DIR/$MODEL_FILE" ]; then
    log "Downloading $MODEL_FILE (~731 MB)..."
    $DL "$MODEL_DIR/$MODEL_FILE" "$HF_BASE/$MODEL_FILE" || warn "Failed: $MODEL_FILE"
  else
    log "LLM model already present."
  fi

  log "LLM model ready."
}

setup_vision_model() {
  log "Setting up LiquidAI LFM2.5-VL-450M model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/vision"
  local HF_BASE="https://huggingface.co/LiquidAI/LFM2.5-VL-450M-GGUF/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping Vision model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  local MODEL_FILE="LFM2.5-VL-450M-Q4_K_M.gguf"

  if [ ! -f "$MODEL_DIR/$MODEL_FILE" ]; then
    log "Downloading $MODEL_FILE (~230 MB)..."
    $DL "$MODEL_DIR/$MODEL_FILE" "$HF_BASE/$MODEL_FILE" || warn "Failed: $MODEL_FILE"
  else
    log "Vision model already present."
  fi

  log "Vision model ready."
}

setup_stt_model() {
  log "Setting up Whisper STT model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/stt"
  local BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
  local TARBALL="sherpa-onnx-whisper-tiny.tar.bz2"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping STT model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/tiny-encoder.int8.onnx" ]; then
    log "Downloading Whisper tiny model (~111 MB)..."
    $DL "$MODEL_DIR/$TARBALL" "$BASE_URL/$TARBALL" || {
      warn "Failed: $TARBALL"
      return
    }

    if [ -f "$MODEL_DIR/$TARBALL" ]; then
      log "Extracting $TARBALL..."
      mkdir -p "$MODEL_DIR/tmp"
      tar -xjf "$MODEL_DIR/$TARBALL" -C "$MODEL_DIR/tmp" --strip-components=1 \
        2>/dev/null || warn "Extraction may be incomplete."
      for f in tiny-encoder.int8.onnx tiny-decoder.int8.onnx tiny-tokens.txt; do
        if [ -f "$MODEL_DIR/tmp/$f" ]; then
          mv "$MODEL_DIR/tmp/$f" "$MODEL_DIR/"
        fi
      done
      rm -rf "$MODEL_DIR/tmp" "$MODEL_DIR/$TARBALL"
    fi
  else
    log "STT model already present."
  fi

  log "STT model ready (~111 MB)."
}

setup_face_model() {
  log "Setting up Face Recognition models..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

main() {
  install_system_deps
  ensure_conan
  configure_conan_profile
  need_cmd git
  need_cmd cmake
  setup_submodules
  setup_tts_model
  setup_llm_model
  setup_vision_model
  setup_stt_model
  setup_face_model
  build_project
  log "All done (profile: $PROFILE). Happy hacking!"
}

main "$@"
