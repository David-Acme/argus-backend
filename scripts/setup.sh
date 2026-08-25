#!/usr/bin/env bash
#
# Argus backend - setup script (Linux).
#
# Automates build with CMake presets. Unifies dev & prod profiles:
#   1. Install system build dependencies (distro aware)
#   2. Install Conan (if missing) and configure the profile for C++20
#   3. Download the Supertonic 3 TTS model (~415 MB)
#   4. Download the LFM2.5-1.2B-Instruct QAD LLM (~696 MB)
#   5. Install Conan dependencies into build/<profile>
#   6. Configure and build with the matching CMake preset
#   7. Create local system/lab configs and start RustFS in Docker
#
# Usage:
#   ./scripts/setup.sh                     # default: dev
#   ./scripts/setup.sh dev                 # dev profile
#   ./scripts/setup.sh prod                # prod profile
#   ./scripts/setup.sh dev --no-build      # install deps only (no compile)
#   ./scripts/setup.sh --storage-only      # configure/start RustFS only
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

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    return 1
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
STORAGE_ONLY=0
ARGS=()

for a in "$@"; do
  case "$a" in
    -h|--help)
      grep '^#' "$0" | sed 's/^#\{1,2\} //'; exit 0 ;;
    --no-build) SKIP_BUILD=1 ;;
    --storage-only) STORAGE_ONLY=1 ;;
    dev|prod)   PROFILE="$a" ;;
    *)          ARGS+=("$a") ;;
  esac
done

case "$PROFILE" in
  dev)  BUILD_TYPE="Debug";   OUTPUT_FOLDER="build/dev";  CMAKE_PRESET="dev" ;;
  prod) BUILD_TYPE="Release"; OUTPUT_FOLDER="build/prod"; CMAKE_PRESET="prod" ;;
esac

log "Profile: $PROFILE  build_type: $BUILD_TYPE  output: $OUTPUT_FOLDER  preset: $CMAKE_PRESET"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="$ROOT/config.toml"
CONFIG_TEMPLATE="$ROOT/config.toml.example"
LABS_CONFIG="$ROOT/labs/config.toml"
LABS_CONFIG_TEMPLATE="$ROOT/labs/config.toml.example"
RUNTIME_DIR="$ROOT/docker/runtime"
SECRET_DIR="$RUNTIME_DIR/secrets"

install_system_deps() {
  log "Detecting distribution and installing build dependencies..."

  local missing=0
  for c in git cmake ninja g++ python3 openssl; do
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
      sudo_if_needed pacman -Syu --needed --noconfirm git cmake ninja gcc python python-pip openssl base-devel \
        vulkan-headers spirv-headers shaderc vulkan-icd-loader ;;
    apt)
      sudo_if_needed apt-get update -y
      sudo_if_needed apt-get install -y --no-install-recommends git cmake ninja-build g++ python3 python3-pip pkg-config openssl ca-certificates \
        libvulkan-dev spirv-headers glslc ;;
    dnf)
      sudo_if_needed dnf install -y git cmake ninja-build gcc-c++ python3 python3-pip openssl \
        vulkan-headers spirv-headers glslc vulkan-loader-devel ;;
    apk)
      sudo_if_needed apk add --no-cache git cmake ninja g++ python3 py3-pip openssl build-base linux-headers ;;
    zypper)
      sudo_if_needed zypper install -y git cmake ninja gcc-c++ python3 python3-pip openssl ;;
    *)
      warn "Unknown distribution. Ensure git cmake ninja g++ python3 pip openssl are installed."
      warn "For GPU offload also install: Vulkan headers/loader, glslc, SPIRV-Headers." ;;
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

  # Local build tweaks for third-party submodules. The submodules point at
  # upstream repos (no push access), so these one-line patches are re-applied
  # on every setup run (idempotent) to keep the build reproducible.
  local F

  F=third_party/ncnn/CMakeLists.txt
  if [ -f "$F" ]; then
    sed -i 's/^\( *\)option(NCNN_BUILD_TOOLS "build tools" OFF)/\1option(NCNN_BUILD_TOOLS "build tools" ON)/' "$F"
    sed -i 's/^\( *\)set(NCNN_BUILD_TOOLS OFF)/\1#set(NCNN_BUILD_TOOLS OFF)/' "$F"
  fi

  F=third_party/sherpa-onnx/cmake/json.cmake
  if [ -f "$F" ]; then
    sed -i 's|^  add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)|  # disabled: nlohmann_json provided by Conan|' "$F"
  fi
}

setup_tts_model() {
  log "Setting up Supertonic 3 TTS model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/tts"
  local ONNX_DIR="$MODEL_DIR/onnx"
  local VOICE_DIR="$MODEL_DIR/voice_styles"
  local HF_BASE="https://huggingface.co/Supertone/supertonic-3/resolve/main"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping TTS model download."
    return
  fi

  mkdir -p "$ONNX_DIR" "$VOICE_DIR"

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

  # License compliance for the Open RAIL-M license (Section 4): ship the
  # license text and an attribution/restrictions notice with the weights.
  if [ ! -f "$MODEL_DIR/LICENSE.openrail-m" ]; then
    log "Downloading Open RAIL-M license..."
    $DL "$MODEL_DIR/LICENSE.openrail-m" \
        "https://huggingface.co/Supertone/supertonic-3/raw/main/LICENSE" || \
      warn "Failed: LICENSE.openrail-m"
  fi

  if [ ! -f "$MODEL_DIR/NOTICE" ]; then
    cat > "$MODEL_DIR/NOTICE" <<'TTS_NOTICE_EOF'
Supertonic 3 — Supertone

Model:      Supertonic 3 (multilingual TTS, ~99M params, ONNX)
Source:     https://huggingface.co/Supertone/supertonic-3
Repo:       https://github.com/supertone-inc/supertonic
License:    BigScience Open RAIL-M License — see LICENSE.openrail-m

Use, modification, distribution and SaaS hosting are permitted provided
recipients receive a copy of the license and the use-based restrictions
(Attachment A) are carried into downstream agreements. No impersonation /
deepfakes, no law-enforcement / justice / immigration / asylum use, no
harmful false information. Output is owned by the user. Provided "AS IS".
TTS_NOTICE_EOF
  fi

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

setup_certs() {
  log "Setting up local PKI (instance CA + server certificate)..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local CERT_DIR="$ROOT/certs"

  need_cmd openssl
  mkdir -p "$CERT_DIR"

  if [ ! -f "$CERT_DIR/ca.pem" ]; then
    log "Generating instance CA (10 years) and server leaf (90 days)..."

    # Instance CA: EC P-256 key + self-signed cert (the per-instance identity).
    # 25 years validity, matching the long-lived root CAs Google ships in
    # Android. The CA never rotates; only the server leaf does.
    openssl ecparam -name prime256v1 -genkey -noout -out "$CERT_DIR/ca.key"
    openssl req -x509 -new -key "$CERT_DIR/ca.key" -sha256 -days 9125 \
      -subj "/CN=Argus Instance CA" -out "$CERT_DIR/ca.pem"

    # Server leaf key.
    openssl ecparam -name prime256v1 -genkey -noout -out "$CERT_DIR/server.key"

    # SANs: advertised hostname, configurable mdns.name, localhost/loopback.
    local MDNS_NAME=""
    if [ -f "$ROOT/config.toml" ]; then
      MDNS_NAME="$(sed -n 's/^[[:space:]]*name *= *"\([^"]*\)".*/\1/p' "$ROOT/config.toml" | head -1)"
    fi
    [ -z "$MDNS_NAME" ] && MDNS_NAME="Argus"
    local SAN="DNS:argus.local,DNS:localhost,IP:127.0.0.1,IP:::1"
    local HN="$(hostname 2>/dev/null)"
    [ -n "$HN" ] && SAN="$SAN,DNS:$HN"
    case "x$MDNS_NAME" in
      x[A-Za-z0-9_-]*) SAN="$SAN,DNS:${MDNS_NAME}" ;;
    esac

    openssl req -new -key "$CERT_DIR/server.key" \
      -subj "/CN=${MDNS_NAME}.local" -out "$CERT_DIR/server.csr"
    openssl x509 -req -in "$CERT_DIR/server.csr" \
      -CA "$CERT_DIR/ca.pem" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
      -sha256 -days 90 -extfile <(printf "subjectAltName=%s" "$SAN") \
      -out "$CERT_DIR/server.pem"
    rm -f "$CERT_DIR/server.csr"

    # Chain file: leaf + CA, so Drogon sends the full chain in the handshake.
    cat "$CERT_DIR/ca.pem" >> "$CERT_DIR/server.pem"

    chmod 600 "$CERT_DIR/ca.key" "$CERT_DIR/server.key"
  else
    log "PKI already present."
  fi

  # Pairing code = first 8 hex chars of the CA SHA-256 fingerprint. Both the
  # server (CertService) and the clients derive it; it gates POST /pairing.
  local FP
  FP="$(openssl x509 -in "$CERT_DIR/ca.pem" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g')"
  echo "${FP:0:8}" > "$CERT_DIR/pairing.code"
  chmod 600 "$CERT_DIR/pairing.code"

  log "CA fingerprint (SHA-256): $FP"
  log "Pairing code: ${FP:0:8}"
}

toml_value() {
  local file="$1"
  local table="$2"
  local key="$3"
  awk -v table="$table" -v key="$key" '
    /^[[:space:]]*\[/ {
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      value = $0
      sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", value)
      sub("[[:space:]]+#.*$", "", value)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
      if (value ~ /^".*"$/) {
        sub(/^"/, "", value)
        sub(/"$/, "", value)
      }
      print value
      exit
    }' "$file"
}

ensure_toml_value() {
  local table="$1"
  local key="$2"
  local value="$3"
  local config="$4"
  local min_length="${5:-1}"
  local max_length="${6:-0}"

  if ! grep -Eq "^[[:space:]]*\\[$table\\][[:space:]]*$" "$config"; then
    printf '\n[%s]\n%s = "%s"\n' "$table" "$key" "$value" >> "$config"
    return
  fi

  local existing
  existing="$(toml_value "$config" "$table" "$key")"
  if [ "${#existing}" -ge "$min_length" ] &&
     { [ "$max_length" -eq 0 ] || [ "${#existing}" -le "$max_length" ]; }; then
    return
  fi

  local temp
  temp="$(mktemp "${config}.tmp.XXXXXX")"
  awk -v table="$table" -v key="$key" -v value="$value" '
    function emit() {
      if (in_table && !found) {
        print key " = \"" value "\""
        found = 1
      }
    }
    /^[[:space:]]*\[/ {
      emit()
      in_table = ($0 ~ "^\\[" table "\\][[:space:]]*$")
      found = 0
      print
      next
    }
    in_table && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      print key " = \"" value "\""
      found = 1
      next
    }
    { print }
    END { emit() }
  ' "$config" > "$temp"
  chmod 600 "$temp"
  mv "$temp" "$config"
}

migrate_legacy_overlay() {
  local legacy="$ROOT/config.local.toml"
  [ -f "$legacy" ] || return 0

  local spec table key value
  for spec in \
      "jwt secret" \
      "jwt refresh_secret" \
      "device fingerprint_secret" \
      "storage mode" \
      "storage.s3 endpoint" \
      "storage.s3 access_key" \
      "storage.s3 secret_key" \
      "storage.s3 region" \
      "storage.s3 bucket"; do
    table="${spec% *}"
    key="${spec##* }"
    value="$(toml_value "$legacy" "$table" "$key")"
    [ -n "$value" ] && ensure_toml_value "$table" "$key" "$value" "$CONFIG"
  done

  rm -f "$legacy"
  log "Migrated the legacy config.local.toml values into config.toml."
}

ensure_secure_file() {
  local path="$1"
  local value="$2"
  [ -n "$value" ] || { err "cannot write an empty secret to $path"; exit 1; }
  if [ -L "$path" ]; then
    err "refusing symbolic link: $path"
    exit 1
  fi
  local temp
  temp="$(mktemp "${path}.tmp.XXXXXX")"
  printf '%s\n' "$value" > "$temp"
  # Compose mounts local secret files without changing their ownership. The
  # parent runtime directory remains 0700, while RustFS needs read access to
  # the mounted file itself.
  chmod 644 "$temp"
  mv "$temp" "$path"
}

ensure_labs_config() {
  [ -f "$LABS_CONFIG_TEMPLATE" ] || {
    err "missing labs config template: $LABS_CONFIG_TEMPLATE"
    exit 1
  }
  if [ ! -f "$LABS_CONFIG" ]; then
    umask 077
    cp "$LABS_CONFIG_TEMPLATE" "$LABS_CONFIG"
    chmod 600 "$LABS_CONFIG"
  fi
}

ensure_local_config() {
  need_cmd openssl
  [ -f "$CONFIG_TEMPLATE" ] || { err "missing config template: $CONFIG_TEMPLATE"; exit 1; }
  if [ ! -f "$CONFIG" ]; then
    umask 077
    cp "$CONFIG_TEMPLATE" "$CONFIG"
    chmod 600 "$CONFIG"
  fi
  migrate_legacy_overlay

  ensure_toml_value jwt secret "$(openssl rand -hex 48)" "$CONFIG"
  ensure_toml_value jwt refresh_secret "$(openssl rand -hex 48)" "$CONFIG"
  ensure_toml_value device fingerprint_secret "$(openssl rand -hex 48)" "$CONFIG"
  ensure_toml_value storage mode "s3" "$CONFIG"
  ensure_toml_value storage.s3 endpoint "http://127.0.0.1:9000" "$CONFIG"
  ensure_toml_value storage.s3 region "us-east-1" "$CONFIG"
  ensure_toml_value storage.s3 bucket "argus-$(openssl rand -hex 10)-private" "$CONFIG"
  ensure_toml_value storage.s3 access_key "$(openssl rand -hex 20 | tr '[:lower:]' '[:upper:]')" "$CONFIG" 40 40
  ensure_toml_value storage.s3 secret_key "$(openssl rand -hex 20)" "$CONFIG" 40 40
  ensure_toml_value storage.rustfs access_key "$(openssl rand -hex 20 | tr '[:lower:]' '[:upper:]')" "$CONFIG"
  ensure_toml_value storage.rustfs secret_key "$(openssl rand -hex 48)" "$CONFIG"
  chmod 600 "$CONFIG"
  ensure_labs_config
  log "System and lab configs are ready."
}

prepare_rustfs_runtime() {
  mkdir -p "$SECRET_DIR"
  chmod 700 "$RUNTIME_DIR" "$SECRET_DIR"
  printf '%s\n' "$(toml_value "$CONFIG" storage.s3 bucket)" > "$RUNTIME_DIR/rustfs-bucket"
  chmod 644 "$RUNTIME_DIR/rustfs-bucket"
  local bucket
  bucket="$(toml_value "$CONFIG" storage.s3 bucket)"
  printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:ListBucket"],"Resource":["arn:aws:s3:::%s"]},{"Effect":"Allow","Action":["s3:GetObject","s3:PutObject","s3:DeleteObject"],"Resource":["arn:aws:s3:::%s/*"]}]}\n' "$bucket" "$bucket" > "$RUNTIME_DIR/rustfs-policy.json"
  chmod 644 "$RUNTIME_DIR/rustfs-policy.json"
  ensure_secure_file "$SECRET_DIR/rustfs_access_key" "$(toml_value "$CONFIG" storage.rustfs access_key)"
  ensure_secure_file "$SECRET_DIR/rustfs_secret_key" "$(toml_value "$CONFIG" storage.rustfs secret_key)"
  ensure_secure_file "$SECRET_DIR/argus_s3_access_key" "$(toml_value "$CONFIG" storage.s3 access_key)"
  ensure_secure_file "$SECRET_DIR/argus_s3_secret_key" "$(toml_value "$CONFIG" storage.s3 secret_key)"
  log "RustFS runtime secrets and bucket descriptor are ready."
}

start_rustfs() {
  need_cmd docker
  docker compose version >/dev/null 2>&1 || {
    err "Docker Compose v2 is required"
    exit 1
  }
  prepare_rustfs_runtime
  log "Starting RustFS and its idempotent bucket initializer..."
  docker compose up -d rustfs rustfs-init
}

setup_llm_model() {
  log "Setting up LiquidAI LFM2.5-1.2B-Instruct QAD model (no thinking, 696 MB)..."
  log "License: LFM Open License v1.0 (see models/llm/LICENSE.lfm1.0)"

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

setup_memory_model() {
  log "Setting up multilingual-e5-small embedding model (int8, 118 MB)..."
  log "License: MIT (see models/memory/LICENSE)"

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

setup_go2rtc() {
  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

setup_vlm_gguf_model() {
  local VARIANT="${ARGUS_VLM_VARIANT:-lfm25}"

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

setup_stt_model() {
  log "Setting up FastConformer Transducer STT model (en+de+es+fr, RTF ~0.02)..."
  log "License: CC-BY-4.0 (NVIDIA NeMo)"

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/stt"
  local BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
  local TARBALL="sherpa-onnx-nemo-fast-conformer-transducer-en-de-es-fr-14288-int8.tar.bz2"
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

  if [ ! -f "$MODEL_DIR/nemo-transducer-encoder.int8.onnx" ]; then
    log "Downloading $TARBALL (~107 MB)..."
    $DL "$MODEL_DIR/$TARBALL" "$BASE_URL/$TARBALL" || {
      warn "Failed: $TARBALL"
      return
    }

    if [ -f "$MODEL_DIR/$TARBALL" ]; then
      log "Extracting $TARBALL..."
      mkdir -p "$MODEL_DIR/tmp"
      tar -xjf "$MODEL_DIR/$TARBALL" -C "$MODEL_DIR/tmp" \
        2>/dev/null || warn "Extraction may be incomplete."
      local TMPDIR
      TMPDIR="$(find "$MODEL_DIR/tmp" -name 'encoder.int8.onnx' -printf '%h' -quit)"
      if [ -z "$TMPDIR" ]; then
        TMPDIR="$MODEL_DIR/tmp"
      fi
      # Rename to the names SttService::createRecognizer() expects.
      if [ -f "$TMPDIR/encoder.int8.onnx" ]; then
        mv "$TMPDIR/encoder.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-encoder.int8.onnx"
      fi
      if [ -f "$TMPDIR/decoder.int8.onnx" ]; then
        mv "$TMPDIR/decoder.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-decoder.int8.onnx"
      fi
      if [ -f "$TMPDIR/joiner.int8.onnx" ]; then
        mv "$TMPDIR/joiner.int8.onnx" \
           "$MODEL_DIR/nemo-transducer-joiner.int8.onnx"
      fi
      if [ -f "$TMPDIR/tokens.txt" ]; then
        mv "$TMPDIR/tokens.txt" \
           "$MODEL_DIR/nemo-transducer-tokens.txt"
      fi
      rm -rf "$MODEL_DIR/tmp" "$MODEL_DIR/$TARBALL"
    fi
  else
    log "STT model already present."
  fi

  log "STT model ready (~107 MB)."
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


setup_extract_model() {
  log "Setting up NuExtract-1.5-tiny structured-extraction model (Q4_K_M, 469 MB)..."
  log "License: MIT (see models/extract/LICENSE)"
  log "Optional: skipping it degrades memory extraction to the lexicon tier."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/extract"
  local BASE_URL="https://huggingface.co/numind/NuExtract-1.5-tiny/resolve/main"
  local MODEL_FILE="NuExtract-1.5-tiny-Q4_K_M.gguf"
  local EXPECTED_BYTES=491400416
  local DL=""

  # HTTP/1.1 with resume: the CDN drops HTTP/2 streams on files this size, and
  # a truncated GGUF fails at load time instead of at download time.
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

setup_vad_model() {
  log "Setting up Silero VAD model..."

  local ROOT
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  local MODEL_DIR="$ROOT/models/vad"
  local BASE_URL="https://github.com/snakers4/silero-vad/raw/v5.0/files"
  local DL=""

  if command -v curl >/dev/null 2>&1; then
    DL="curl -L --retry 3 --progress-bar -o"
  elif command -v wget >/dev/null 2>&1; then
    DL="wget --retry-connrefused --waitretry=3 --show-progress -O"
  else
    warn "Neither curl nor wget found; skipping VAD model download."
    return
  fi

  mkdir -p "$MODEL_DIR"

  if [ ! -f "$MODEL_DIR/silero_vad.onnx" ]; then
    log "Downloading silero_vad.onnx (~2.3 MB)..."
    $DL "$MODEL_DIR/silero_vad.onnx" "$BASE_URL/silero_vad.onnx" || \
      warn "Failed: silero_vad.onnx"
  else
    log "VAD model already present."
  fi

  log "VAD model ready (~2.3 MB)."
}

main() {
  ensure_local_config
  start_rustfs

  if [ "$STORAGE_ONLY" -eq 1 ]; then
    log "RustFS setup complete; native backend development remains unchanged."
    exit 0
  fi

  # Hardware detection installs the GPU/video/audio stack for this specific
  # host and writes scripts/.hw-profile. It asks for sudo only if something is
  # actually missing. install_system_deps stays as the minimal fallback.
  if [ -x "$(dirname "$0")/detect-hardware.sh" ]; then
    "$(dirname "$0")/detect-hardware.sh" ${ARGUS_ASSUME_YES:+-y} || \
      warn "Hardware detection failed; falling back to the base dependency set."
  fi
  install_system_deps
  ensure_conan
  configure_conan_profile
  need_cmd git
  need_cmd cmake
  setup_submodules
  setup_certs
  setup_tts_model
  setup_llm_model
  setup_vlm_gguf_model
  setup_go2rtc
  setup_stt_model
  setup_face_model
  setup_vad_model
  setup_extract_model
  setup_memory_model
  build_project
  log "All done (profile: $PROFILE). Happy hacking!"
}

main "$@"
