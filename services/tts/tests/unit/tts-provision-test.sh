#!/usr/bin/env bash
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../scripts" && pwd)/provision.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILURES=0

plan() {
  local config="$1"
  shift
  env -i PATH="$PATH" HOME="$WORK" ARGUS_TTS_CONFIG="$config" ARGUS_TTS_POCKET_DIR="$WORK/pocket" "$@" \
    bash "$SCRIPT" --plan 2>/dev/null
}

config() {
  local file="$WORK/$1.toml"
  shift
  printf '[tts]\n' > "$file"
  printf '%s\n' "$@" >> "$file"
  printf '%s\n' "$file"
}

expect() {
  local name="$1" actual="$2" expected="$3"
  if [ "$actual" != "$expected" ]; then
    printf 'FAIL %s\nexpected:\n%s\nactual:\n%s\n' "$name" "$expected" "$actual" >&2
    FAILURES=$((FAILURES + 1))
  else
    printf 'ok   %s\n' "$name"
  fi
}

expect "defaults install the 24-layer Spanish and English models with the Commons voices" \
  "$(plan "$WORK/missing.toml")" \
  "variant es-quality
voice es-quality lola
variant en
voice en alba"

expect "the non-commercial opt-in adds jean to every selected variant" \
  "$(plan "$WORK/missing.toml" ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1)" \
  "variant es-quality
voice es-quality jean
voice es-quality lola
variant en
voice en jean
voice en alba"

expect "a language answered by Supertonic installs no Pocket model" \
  "$(plan "$(config supertonic 'engine_es = "supertonic"')")" \
  "variant en
voice en alba"

expect "the fast Spanish variant follows the configuration" \
  "$(plan "$(config fast 'pocket_variant_es = "fast"' 'engine_en = "supertonic"')")" \
  "variant es-fast
voice es-fast lola"

expect "ARGUS_TTS_POCKET_QUALITY=0 keeps the 6-layer Spanish model" \
  "$(plan "$WORK/missing.toml" ARGUS_TTS_POCKET_QUALITY=0 ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1)" \
  "variant es-fast
voice es-fast jean
voice es-fast lola
variant en
voice en jean
voice en alba"

expect "a configured Commons voice is installed next to the fallback" \
  "$(plan "$(config voices 'pocket_voice_es = "michael"' 'pocket_voice_en = "george"')")" \
  "variant es-quality
voice es-quality michael
voice es-quality lola
variant en
voice en george
voice en alba"

expect "an unknown configured voice falls back to the default" \
  "$(plan "$(config unknown 'pocket_voice_en = "cosette"' 'engine_es = "supertonic"')" ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1)" \
  "variant en
voice en jean
voice en alba"

expect "an explicit variant list overrides the configuration" \
  "$(plan "$WORK/missing.toml" ARGUS_TTS_POCKET_VARIANTS="en es-fast nonsense")" \
  "variant en
voice en alba
variant es-fast
voice es-fast lola"

expect "ARGUS_TTS_POCKET=0 selects nothing" "$(plan "$WORK/missing.toml" ARGUS_TTS_POCKET=0)" ""

catalog="$(env -i PATH="$PATH" HOME="$WORK" bash "$SCRIPT" --catalog)"
expect "the catalog names the three variants" "$(grep -c '^variant ' <<<"$catalog")" "3"
expect "the catalog names nine voices per variant" "$(grep -c '^voice ' <<<"$catalog")" "27"
expect "jean is the only non-commercial voice" \
  "$(awk '$1 == "voice" && $5 ~ /-nc-/ { print $2 " " $3 }' <<<"$catalog")" \
  "es-fast jean
es-quality jean
en jean"
expect "every catalog size is a positive byte count" \
  "$(awk '($1 == "variant" && $3 !~ /^[1-9][0-9]*$/) || ($1 == "voice" && $4 !~ /^[1-9][0-9]*$/)' <<<"$catalog")" ""

refused() {
  local status=0
  env -i PATH="$PATH" HOME="$WORK" ARGUS_TTS_POCKET_DIR="$WORK/pocket" bash "$SCRIPT" "$@" >/dev/null 2>&1 || status=$?
  printf '%s\n' "$status"
}

expect "an unknown variant is refused before anything runs" "$(refused --variant ../es-fast)" "2"
expect "an unknown voice is refused before anything runs" "$(refused --voice en:../../x)" "2"
expect "a voice of another language is refused" "$(refused --voice en:lola)" "2"
expect "a non-commercial voice needs the opt-in" "$(refused --voice en:jean)" "1"
expect "nothing was written for refused requests" "$(find "$WORK/pocket" -name '*.safetensors' 2>/dev/null | wc -l)" "0"

if [ "$FAILURES" -gt 0 ]; then
  printf '%s provisioning check(s) failed\n' "$FAILURES" >&2
  exit 1
fi
