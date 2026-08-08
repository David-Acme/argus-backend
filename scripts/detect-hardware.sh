#!/usr/bin/env bash
# Detects the host (CPU, RAM, GPU, video decode, OS) and installs everything
# Argus needs to use that hardware to its full extent. Writes the result to
# scripts/.hw-profile so setup.sh and CMake can consume it.
#
# sudo is requested ONLY when a package actually has to be installed.
#
# Deliberately NOT using `set -e`: this script's whole job is probing things
# that may legitimately be absent (no /dev/dri, no lspci, no nvidia-smi), and
# every `[ test ] && assignment` would abort the run under -e.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="$ROOT/scripts/.hw-profile"

log()  { printf '\033[1;34m[hw]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; }
ok()   { printf '\033[1;32m[ok]\033[0m %s\n' "$*"; }

DRY_RUN=0
ASSUME_YES=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    -y|--yes)  ASSUME_YES=1 ;;
    -h|--help)
      cat <<'USAGE'
Usage: scripts/detect-hardware.sh [--dry-run] [-y|--yes]

  --dry-run   Detect and report, install nothing.
  -y, --yes   Do not prompt before installing packages.

Writes scripts/.hw-profile with the detected capabilities.
USAGE
      exit 0 ;;
  esac
done

# ── sudo, only when something is actually missing ────────────────────────────

SUDO=""
need_sudo() {
  if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
    return 0
  fi
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
    return 0
  fi
  warn "No sudo available and not running as root; cannot install packages."
  return 1
}

confirm() {
  [ "$ASSUME_YES" -eq 1 ] && return 0
  printf '%s [Y/n] ' "$1"
  read -r reply </dev/tty || return 1
  case "$reply" in [nN]*) return 1 ;; *) return 0 ;; esac
}

# ── OS / package manager ─────────────────────────────────────────────────────

OS_NAME="unknown"; OS_VERSION=""; PKG=""
detect_os() {
  case "$(uname -s)" in
    Darwin)
      OS_NAME="macos"; OS_VERSION="$(sw_vers -productVersion 2>/dev/null || echo '')"
      command -v brew >/dev/null 2>&1 && PKG="brew"
      return ;;
    Linux) ;;
    *) OS_NAME="$(uname -s)"; return ;;
  esac

  if [ -r /etc/os-release ]; then
    . /etc/os-release
    OS_NAME="${ID:-linux}"; OS_VERSION="${VERSION_ID:-}"
    case "${ID:-},${ID_LIKE:-}" in
      *arch*|*manjaro*|*endeavour*) PKG="pacman" ;;
      *debian*|*ubuntu*|*mint*|*pop*) PKG="apt" ;;
      *fedora*|*rhel*|*centos*|*rocky*|*alma*) PKG="dnf" ;;
      *alpine*) PKG="apk" ;;
      *suse*|*opensuse*) PKG="zypper" ;;
    esac
  fi
  [ -z "$PKG" ] && for c in pacman apt-get dnf apk zypper; do
    command -v "$c" >/dev/null 2>&1 && { PKG="${c%%-*}"; break; }
  done
}

pkg_installed() {
  case "$PKG" in
    pacman)  pacman -Qq "$1" >/dev/null 2>&1 ;;
    apt)     dpkg -s "$1" >/dev/null 2>&1 ;;
    dnf)     rpm -q "$1" >/dev/null 2>&1 ;;
    apk)     apk info -e "$1" >/dev/null 2>&1 ;;
    zypper)  rpm -q "$1" >/dev/null 2>&1 ;;
    brew)    brew list --formula "$1" >/dev/null 2>&1 ;;
    *)       return 1 ;;
  esac
}

pkg_exists() {
  case "$PKG" in
    pacman)  pacman -Si "$1" >/dev/null 2>&1 ;;
    apt)     apt-cache show "$1" >/dev/null 2>&1 ;;
    dnf)     dnf info "$1" >/dev/null 2>&1 ;;
    apk)     apk info "$1" >/dev/null 2>&1 ;;
    zypper)  zypper info "$1" >/dev/null 2>&1 ;;
    brew)    brew info "$1" >/dev/null 2>&1 ;;
    *)       return 1 ;;
  esac
}

pkg_install() {
  [ "$#" -eq 0 ] && return 0
  case "$PKG" in
    pacman) $SUDO pacman -S --needed --noconfirm "$@" ;;
    apt)    $SUDO apt-get update -qq && $SUDO apt-get install -y --no-install-recommends "$@" ;;
    dnf)    $SUDO dnf install -y "$@" ;;
    apk)    $SUDO apk add --no-cache "$@" ;;
    zypper) $SUDO zypper install -y "$@" ;;
    brew)   brew install "$@" ;;
    *)      return 1 ;;
  esac
}

# ── CPU / RAM ────────────────────────────────────────────────────────────────

CPU_MODEL="unknown"; CPU_CORES=1; CPU_THREADS=1
CPU_AVX2=0; CPU_AVX512=0; CPU_F16C=0; CPU_NEON=0
RAM_MB=0; SWAP_MB=0

detect_cpu() {
  CPU_THREADS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  if [ "$OS_NAME" = "macos" ]; then
    CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    CPU_CORES="$(sysctl -n hw.physicalcpu 2>/dev/null || echo "$CPU_THREADS")"
    RAM_MB=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 ))
    [ "$(uname -m)" = "arm64" ] && CPU_NEON=1
    return
  fi

  [ -r /proc/cpuinfo ] && {
    CPU_MODEL="$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo)"
    [ -z "$CPU_MODEL" ] && CPU_MODEL="$(awk -F': ' '/^Model/{print $2; exit}' /proc/cpuinfo)"
    local flags; flags="$(awk -F': ' '/^flags|^Features/{print $2; exit}' /proc/cpuinfo)"
    case " $flags " in *" avx2 "*) CPU_AVX2=1 ;; esac
    case " $flags " in *" avx512f "*) CPU_AVX512=1 ;; esac
    case " $flags " in *" f16c "*) CPU_F16C=1 ;; esac
    case " $flags " in *" neon "*|*" asimd "*) CPU_NEON=1 ;; esac
  }
  CPU_CORES="$(LC_ALL=C lscpu 2>/dev/null | awk -F': +' '/^Core\(s\) per socket/{c=$2} /^Socket\(s\)/{s=$2} END{if (c&&s) print c*s}')"
  [ -z "$CPU_CORES" ] && CPU_CORES="$CPU_THREADS"

  [ -r /proc/meminfo ] && {
    RAM_MB=$(( $(awk '/^MemTotal/{print $2}' /proc/meminfo) / 1024 ))
    SWAP_MB=$(( $(awk '/^SwapTotal/{print $2}' /proc/meminfo) / 1024 ))
  }
}

# ── GPU / Vulkan / video decode ──────────────────────────────────────────────

GPU_VENDOR="none"; GPU_MODEL=""; GPU_DRIVER=""; GPU_DISCRETE=0
VIDEO_ACCEL="none"; VIDEO_DEVICE=""
HAS_VULKAN_LOADER=0; HAS_GLSLC=0; HAS_SPIRV_HEADERS=0; HAS_VULKAN_HEADERS=0
HAS_CUDA=0; HAS_NVCC=0; HAS_ROCM=0; VULKAN_DEVICES=0

detect_gpu() {
  if [ "$OS_NAME" = "macos" ]; then
    GPU_VENDOR="apple"; VIDEO_ACCEL="videotoolbox"
    GPU_MODEL="$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{print $2; exit}')"
    return
  fi

  for node in /dev/dri/renderD*; do
    [ -e "$node" ] || continue
    local name drv
    name="$(basename "$node")"
    drv="$(awk -F= '/^DRIVER=/{print $2}' "/sys/class/drm/$name/device/uevent" 2>/dev/null || true)"
    case "$drv" in
      nvidia*)       GPU_VENDOR="nvidia"; VIDEO_ACCEL="nvdec" ;;
      i915|xe)       GPU_VENDOR="intel";  VIDEO_ACCEL="qsv" ;;
      amdgpu|radeon) GPU_VENDOR="amd";    VIDEO_ACCEL="vaapi" ;;
      *) [ "$GPU_VENDOR" = "none" ] && { GPU_VENDOR="${drv:-unknown}"; VIDEO_ACCEL="vaapi"; } ;;
    esac
    GPU_DRIVER="${drv:-}"; VIDEO_DEVICE="$node"
    [ "$GPU_VENDOR" != "none" ] && break
  done

  if command -v lspci >/dev/null 2>&1; then
    GPU_MODEL="$(lspci 2>/dev/null | grep -iE 'vga|3d controller|display' | head -1 | sed 's/.*: //')"
    case "$GPU_MODEL" in
      *Discrete*|*GeForce*|*Radeon\ RX*|*Quadro*|*Tesla*|*Arc*) GPU_DISCRETE=1 ;;
    esac
  fi

  command -v nvidia-smi >/dev/null 2>&1 && { HAS_CUDA=1; GPU_DISCRETE=1; }
  command -v nvcc       >/dev/null 2>&1 && HAS_NVCC=1
  [ -x /usr/local/cuda/bin/nvcc ] && HAS_NVCC=1
  command -v rocminfo   >/dev/null 2>&1 && HAS_ROCM=1

  # One `ls` over several globs returns non-zero if ANY glob fails to match,
  # so each candidate is probed on its own.
  for lib in /usr/lib/libvulkan.so* /usr/lib64/libvulkan.so* \
             /usr/lib/*/libvulkan.so* /usr/local/lib/libvulkan.so*; do
    [ -e "$lib" ] && { HAS_VULKAN_LOADER=1; break; }
  done
  command -v glslc >/dev/null 2>&1 && HAS_GLSLC=1
  for d in /usr/include/spirv /usr/include/SPIRV-Headers /usr/share/cmake/SPIRV-Headers \
           /usr/lib/cmake/SPIRV-Headers /usr/lib64/cmake/SPIRV-Headers; do
    [ -e "$d" ] && HAS_SPIRV_HEADERS=1
  done
  [ -e /usr/include/vulkan/vulkan.h ] && HAS_VULKAN_HEADERS=1

  # Files existing is not the same as Vulkan working: a missing or broken ICD
  # enumerates zero devices and every GPU path silently falls back to CPU.
  if command -v vulkaninfo >/dev/null 2>&1; then
    VULKAN_DEVICES="$(vulkaninfo --summary 2>/dev/null | grep -c 'deviceName' || echo 0)"
  fi
}

# ── package name mapping ─────────────────────────────────────────────────────

want_packages() {
  local -n out=$1
  out=()

  local base_pacman=(git cmake ninja gcc python python-pip openssl base-devel pkgconf)
  local base_apt=(git cmake ninja-build g++ python3 python3-pip pkg-config openssl ca-certificates)
  local base_dnf=(git cmake ninja-build gcc-c++ python3 python3-pip openssl pkgconf)
  local base_apk=(git cmake ninja g++ python3 py3-pip openssl build-base linux-headers)
  local base_zyp=(git cmake ninja gcc-c++ python3 python3-pip openssl pkg-config)
  local base_brew=(git cmake ninja python openssl pkg-config)

  case "$PKG" in
    pacman) out+=("${base_pacman[@]}") ;;
    apt)    out+=("${base_apt[@]}") ;;
    dnf)    out+=("${base_dnf[@]}") ;;
    apk)    out+=("${base_apk[@]}") ;;
    zypper) out+=("${base_zyp[@]}") ;;
    brew)   out+=("${base_brew[@]}") ;;
  esac

  # ffmpeg: needed for hardware-accelerated H.264 decode of the camera stream.
  case "$PKG" in
    pacman) out+=(ffmpeg) ;;
    apt)    out+=(libavcodec-dev libavformat-dev libavutil-dev libswscale-dev) ;;
    dnf)    out+=(ffmpeg-free-devel) ;;
    apk)    out+=(ffmpeg-dev) ;;
    zypper) out+=(ffmpeg-devel) ;;
    brew)   out+=(ffmpeg) ;;
  esac

  # Vulkan toolchain: unlocks GPU offload for the LLM and the VLM.
  if [ "$OS_NAME" != "macos" ]; then
    case "$PKG" in
      pacman) out+=(vulkan-headers spirv-headers shaderc vulkan-icd-loader) ;;
      apt)    out+=(libvulkan-dev spirv-headers glslc) ;;
      dnf)    out+=(vulkan-headers spirv-headers glslc vulkan-loader-devel) ;;
      apk)    out+=(vulkan-headers vulkan-loader-dev shaderc) ;;
      zypper) out+=(vulkan-headers spirv-headers shaderc vulkan-loader) ;;
    esac
    # Vendor userspace driver, so Vulkan actually finds a device.
    case "$GPU_VENDOR:$PKG" in
      amd:pacman)    out+=(vulkan-radeon libva-mesa-driver) ;;
      amd:apt)       out+=(mesa-vulkan-drivers va-driver-all) ;;
      amd:dnf)       out+=(mesa-vulkan-drivers mesa-va-drivers) ;;
      amd:apk)       out+=(mesa-vulkan-ati mesa-va-gallium) ;;
      amd:zypper)    out+=(libvulkan_radeon Mesa-libva) ;;
      intel:pacman)  out+=(vulkan-intel intel-media-driver) ;;
      intel:apt)     out+=(mesa-vulkan-drivers intel-media-va-driver) ;;
      intel:dnf)     out+=(mesa-vulkan-drivers intel-media-driver) ;;
      intel:apk)     out+=(mesa-vulkan-intel intel-media-driver) ;;
      intel:zypper)  out+=(libvulkan_intel intel-media-driver) ;;
      nvidia:pacman) out+=(nvidia-utils libva-nvidia-driver) ;;
      nvidia:apt)    out+=(nvidia-driver libnvidia-egl-wayland1) ;;
      nvidia:dnf)    out+=(akmod-nvidia xorg-x11-drv-nvidia-cuda) ;;
      nvidia:apk)    out+=(nvidia-drivers) ;;
      nvidia:zypper) out+=(nvidia-video-G06) ;;
    esac

    # Diagnostics: vulkaninfo is how a failing GPU path gets diagnosed.
    case "$PKG" in
      pacman) out+=(vulkan-tools) ;;
      apt)    out+=(vulkan-tools) ;;
      dnf)    out+=(vulkan-tools) ;;
      apk)    out+=(vulkan-tools) ;;
      zypper) out+=(vulkan-tools) ;;
    esac

    # CUDA toolkit: on NVIDIA, llama.cpp built with GGML_CUDA beats the Vulkan
    # path by a wide margin, so the toolkit is worth installing.
    if [ "$GPU_VENDOR" = "nvidia" ]; then
      case "$PKG" in
        pacman) out+=(cuda) ;;
        apt)    out+=(nvidia-cuda-toolkit) ;;
        dnf)    out+=(cuda-toolkit) ;;
        zypper) out+=(cuda-toolkit) ;;
      esac
    fi
  fi

  # Microphone capture for the voice pipeline.
  case "$PKG" in
    pacman) out+=(portaudio) ;;
    apt)    out+=(portaudio19-dev) ;;
    dnf)    out+=(portaudio-devel) ;;
    apk)    out+=(portaudio-dev) ;;
    zypper) out+=(portaudio-devel) ;;
    brew)   out+=(portaudio) ;;
  esac
}

# ── report + profile ─────────────────────────────────────────────────────────

TIER="minimal"; GPU_BACKEND="cpu"

# Picks the single best backend the host can actually run, mirroring the
# precedence in third_party/CMakeLists.txt: CUDA > Vulkan > CPU. Never mixes
# vendors: only the driver stack matching the detected GPU gets installed.
derive_backend() {
  if [ "$HAS_NVCC" -eq 1 ] && [ "$GPU_VENDOR" = "nvidia" ]; then
    GPU_BACKEND="cuda"
  elif [ "$HAS_VULKAN_LOADER" -eq 1 ] && [ "$HAS_GLSLC" -eq 1 ] && \
       [ "$HAS_SPIRV_HEADERS" -eq 1 ] && [ "${VULKAN_DEVICES:-0}" -gt 0 ]; then
    GPU_BACKEND="vulkan"
  else
    GPU_BACKEND="cpu"
  fi
}

derive_tier() {
  local vulkan_ready=0
  { [ "$HAS_VULKAN_LOADER" -eq 1 ] && [ "$HAS_GLSLC" -eq 1 ] && \
    [ "$HAS_SPIRV_HEADERS" -eq 1 ]; } && vulkan_ready=1
  # An enumerated device is what actually matters; headers alone prove nothing.
  [ "${VULKAN_DEVICES:-0}" -eq 0 ] && [ "$HAS_NVCC" -eq 0 ] && vulkan_ready=0

  if [ "$CPU_CORES" -le 2 ] || [ "$RAM_MB" -lt 4096 ]; then
    TIER="minimal"
  elif [ "$vulkan_ready" -eq 1 ] && [ "$GPU_DISCRETE" -eq 1 ] && [ "$CPU_CORES" -ge 8 ]; then
    TIER="high"
  elif [ "$vulkan_ready" -eq 1 ] && [ "$CPU_CORES" -ge 6 ] && [ "$RAM_MB" -ge 8192 ]; then
    TIER="balanced"
  else
    TIER="low"
  fi
}

write_profile() {
  cat > "$PROFILE" <<EOF
# Generated by scripts/detect-hardware.sh on $(date -Iseconds)
ARGUS_OS=$OS_NAME
ARGUS_OS_VERSION=$OS_VERSION
ARGUS_PKG=$PKG
ARGUS_CPU_MODEL=$CPU_MODEL
ARGUS_CPU_CORES=$CPU_CORES
ARGUS_CPU_THREADS=$CPU_THREADS
ARGUS_CPU_AVX2=$CPU_AVX2
ARGUS_CPU_AVX512=$CPU_AVX512
ARGUS_CPU_NEON=$CPU_NEON
ARGUS_RAM_MB=$RAM_MB
ARGUS_SWAP_MB=$SWAP_MB
ARGUS_GPU_VENDOR=$GPU_VENDOR
ARGUS_GPU_MODEL=$GPU_MODEL
ARGUS_GPU_DRIVER=$GPU_DRIVER
ARGUS_GPU_DISCRETE=$GPU_DISCRETE
ARGUS_VIDEO_ACCEL=$VIDEO_ACCEL
ARGUS_VIDEO_DEVICE=$VIDEO_DEVICE
ARGUS_VULKAN_LOADER=$HAS_VULKAN_LOADER
ARGUS_VULKAN_HEADERS=$HAS_VULKAN_HEADERS
ARGUS_GLSLC=$HAS_GLSLC
ARGUS_SPIRV_HEADERS=$HAS_SPIRV_HEADERS
ARGUS_CUDA=$HAS_CUDA
ARGUS_NVCC=$HAS_NVCC
ARGUS_ROCM=$HAS_ROCM
ARGUS_VULKAN_DEVICES=$VULKAN_DEVICES
ARGUS_TIER=$TIER
ARGUS_GPU_BACKEND=$GPU_BACKEND
EOF
}

print_report() {
  echo
  log "System"
  printf '  os            : %s %s (%s)\n' "$OS_NAME" "$OS_VERSION" "${PKG:-no package manager}"
  printf '  cpu           : %s\n' "$CPU_MODEL"
  printf '  cores/threads : %s / %s\n' "$CPU_CORES" "$CPU_THREADS"
  printf '  isa           : avx2=%s avx512=%s neon=%s\n' "$CPU_AVX2" "$CPU_AVX512" "$CPU_NEON"
  printf '  memory        : %s MB ram, %s MB swap\n' "$RAM_MB" "$SWAP_MB"
  printf '  gpu           : %s%s\n' "${GPU_MODEL:-none}" \
         "$([ "$GPU_DISCRETE" -eq 1 ] && echo ' [discrete]' || echo '')"
  printf '  gpu driver    : %s\n' "${GPU_DRIVER:-n/a}"
  printf '  video decode  : %s %s\n' "$VIDEO_ACCEL" "$VIDEO_DEVICE"
  printf '  vulkan        : loader=%s headers=%s glslc=%s spirv-headers=%s\n' \
         "$HAS_VULKAN_LOADER" "$HAS_VULKAN_HEADERS" "$HAS_GLSLC" "$HAS_SPIRV_HEADERS"
  printf '  vulkan devices: %s (enumerated)\n' "$VULKAN_DEVICES"
  printf '  cuda / nvcc   : %s / %s\n' "$HAS_CUDA" "$HAS_NVCC"
  printf '  rocm          : %s\n' "$HAS_ROCM"
  echo
  log "Selected GPU backend: $GPU_BACKEND"
  case "$GPU_BACKEND" in
    cuda)   echo "  llama.cpp builds with GGML_CUDA; LLM and VLM offload to the NVIDIA GPU" ;;
    vulkan) echo "  llama.cpp builds with GGML_VULKAN; LLM and VLM offload to ${GPU_MODEL:-the GPU}" ;;
    cpu)    echo "  no usable GPU stack; everything runs on CPU (still correct, just slower)" ;;
  esac
  log "Resolved tier: $TIER"
  case "$TIER" in
    minimal) echo "  detector 512@2fps on CPU, VLM off, 350M LLM, KV cache q4_0" ;;
    low)     echo "  detector 512@4fps on CPU, VLM on demand, 1.2B LLM, KV cache q8_0" ;;
    balanced)echo "  detector 512@8fps on Vulkan, VLM on, 1.2B LLM offloaded, KV cache f16" ;;
    high)    echo "  detector 640@12fps on Vulkan, VLM on, 1.2B LLM fully offloaded, KV f16" ;;
  esac
}

# ── main ─────────────────────────────────────────────────────────────────────

detect_os
detect_cpu
detect_gpu
derive_backend
derive_tier
print_report

if [ -z "$PKG" ]; then
  warn "No supported package manager detected; skipping installation."
  write_profile
  exit 0
fi

declare -a WANTED
want_packages WANTED

declare -a MISSING
for p in "${WANTED[@]}"; do
  pkg_installed "$p" && continue
  pkg_exists "$p" || { warn "package not in repos, skipping: $p"; continue; }
  MISSING+=("$p")
done

echo
if [ "${#MISSING[@]}" -eq 0 ]; then
  ok "Every package Argus can use is already installed."
else
  log "Missing packages (${#MISSING[@]}):"
  printf '    %s\n' "${MISSING[@]}"
  if [ "$DRY_RUN" -eq 1 ]; then
    warn "--dry-run: nothing installed."
  elif ! confirm "Install them now?"; then
    warn "Skipped by user."
  elif need_sudo; then
    log "Installing (sudo needed only for this step)..."
    if pkg_install "${MISSING[@]}"; then
      ok "Packages installed."
      detect_gpu
      derive_backend
      derive_tier
      echo
      log "Re-detected after install:"
      printf '  vulkan        : loader=%s headers=%s glslc=%s spirv-headers=%s\n' \
             "$HAS_VULKAN_LOADER" "$HAS_VULKAN_HEADERS" "$HAS_GLSLC" "$HAS_SPIRV_HEADERS"
      log "Selected GPU backend: $GPU_BACKEND"
      log "Resolved tier: $TIER"
    else
      err "Installation failed; the build will fall back to CPU where needed."
    fi
  fi
fi

write_profile
echo
ok "Profile written to scripts/.hw-profile"
[ "$HAS_SPIRV_HEADERS" -eq 1 ] && [ "$HAS_GLSLC" -eq 1 ] || \
  warn "Vulkan offload stays OFF until glslc + SPIRV-Headers are present."
