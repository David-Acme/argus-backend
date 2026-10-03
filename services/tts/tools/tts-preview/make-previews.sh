#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../../scripts/lib/common.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
TOOL="$ROOT/services/tts/build/dev/tools/tts-preview/argus-tts-preview"
MODELS="$ROOT/models/tts"
CONFIG="$ROOT/services/tts/config.toml.example"
OUT="$ROOT/../frontend/src/assets/audio/tts-previews"
SEED=7
TEXT_ES="Hola, soy Argus. Tu reunión empieza a las 16:30."
TEXT_EN="Hi, I'm Argus. Your meeting starts at 4:30 pm."
SPANISH_VOICES=(jean lola alba eve fantine giovanni marius javert michael)
ENGLISH_VOICES=(jean alba eve jane mary marius javert michael george)

usage() {
  cat <<'USAGE_EOF'
Usage: make-previews.sh [--tool <argus-tts-preview>] [--models <models/tts>] [--out <dir>]

Synthesizes the voice previews the app plays in Configuración, with the
production engines, the shipped config.toml.example defaults and a fixed
seed, and encodes each clip twice: Opus in
Ogg (web and desktop) and AAC in M4A (Android and iOS). The same Spanish
and English sentence is used for every option:
  Supertonic es/en with M3, the voice every caller requests;
  Pocket es fast, es quality and en with every selectable voice, jean included
  (CC BY-NC 4.0, non-commercial).
Every Pocket variant and voice must be installed first, for example:
  ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1 services/tts/scripts/provision.sh --voice es-fast:jean
Requires ffmpeg with libopus and the native AAC encoder.

Defaults: the dev build of argus-tts-preview, models/tts, and the frontend's
src/assets/audio/tts-previews next to this repository.
USAGE_EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tool) TOOL="$2"; shift 2 ;;
    --models) MODELS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) err "Unknown option: $1"; usage >&2; exit 2 ;;
  esac
done

need_cmd ffmpeg
[ -x "$TOOL" ] || { err "argus-tts-preview not found at $TOOL; build tts first (./scripts/build-all.sh dev --only tts)."; exit 1; }

manifest() {
  local voice
  printf 'supertonic-es-m3 es supertonic - M3\n'
  printf 'supertonic-en-m3 en supertonic - M3\n'
  for voice in "${SPANISH_VOICES[@]}"; do
    printf 'pocket-es-fast-%s es pocket fast %s\n' "$voice" "$voice"
    printf 'pocket-es-quality-%s es pocket quality %s\n' "$voice" "$voice"
  done
  for voice in "${ENGLISH_VOICES[@]}"; do
    printf 'pocket-en-%s en pocket - %s\n' "$voice" "$voice"
  done
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

log "Synthesizing $(manifest | wc -l) previews with seed $SEED..."
manifest | "$TOOL" --config "$CONFIG" --models "$MODELS" --out "$WORK" --text-es "$TEXT_ES" --text-en "$TEXT_EN" --seed "$SEED"

mkdir -p "$OUT"
for wave in "$WORK"/*.wav; do
  name="$(basename "$wave" .wav)"
  ffmpeg -nostdin -loglevel error -y -i "$wave" -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
    -ac 1 -ar 24000 -c:a libopus -b:a 24k -application voip "$OUT/$name.ogg"
  ffmpeg -nostdin -loglevel error -y -i "$wave" -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
    -ac 1 -ar 22050 -c:a aac -b:a 32k "$OUT/$name.m4a"
done

ogg_bytes="$(cat "$OUT"/*.ogg | wc -c)"
m4a_bytes="$(cat "$OUT"/*.m4a | wc -c)"
log "Wrote $(find "$OUT" -name '*.ogg' | wc -l) previews to $OUT: Opus $((ogg_bytes / 1024)) KiB, AAC $((m4a_bytes / 1024)) KiB."
