#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../../scripts/lib/common.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOLS_DIR="$ROOT/services/tts/tools"
POCKET_DIR="$ROOT/models/tts/pocket"
POCKET_REPO="https://huggingface.co/kyutai/pocket-tts-without-voice-cloning/resolve"
POCKET_VOICE_REVISION="1e08e6a23401048648a9fdcfde2f89348215c2a7"
POCKET_SOURCE="git+https://github.com/kyutai-labs/pocket-tts@41cbc84af539ea78a804ffca5f9c6edc1a22ce44"
POCKET_WORK="${ARGUS_POCKET_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/argus/pocket-export}"
DL=""

setup_tts_model() {
  log "Setting up Supertonic 3 TTS model..."

  local MODEL_DIR="$ROOT/models/tts"
  local ONNX_DIR="$MODEL_DIR/onnx"
  local VOICE_DIR="$MODEL_DIR/voice_styles"
  local HF_BASE="https://huggingface.co/Supertone/supertonic-3/resolve/main"

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

fetch_pinned() {
  local url="$1" target="$2" expected="$3"
  if [ -f "$target" ] && [ "$(sha256_file "$target")" = "$expected" ]; then
    return 0
  fi
  rm -f "$target" "$target.part"
  mkdir -p "$(dirname "$target")"
  if ! curl -fL --retry 3 --silent --show-error -o "$target.part" "$url"; then
    rm -f "$target.part"
    warn "Download failed: $(basename "$target")"
    return 1
  fi
  local actual
  actual="$(sha256_file "$target.part")"
  if [ "$actual" != "$expected" ]; then
    rm -f "$target.part"
    warn "Checksum mismatch for $(basename "$target") (expected $expected, got $actual)."
    return 1
  fi
  mv "$target.part" "$target"
}

pocket_voices() {
  case "$1" in
    es-fast)
      printf '%s\n' \
        "lola 967b8f08639345ff5ad1a912a5542fe421518c3f80b87d52078c6726348ab1e7" \
        "alba 3054baa9d54a47667ff333a95f4d3ed9dc7d31ada1fe99cee334d2bb6f1a5029" \
        "eve 034a2fb66ce02cb19a246f64d0f5a1c4a3a04688b067fa6ba3c7e61b5c15dab0" \
        "fantine 3156e3afa0457d0ab3f7440b9eb59bc3c41b19392652e6dca402367e2a7b6f79" \
        "giovanni 3f4b9e1779eeb65311494e6571a024725d8e2027ebd43b4dc924c11f347b86be" \
        "marius 9f5efb293cb02ffe40e8ae8e54633a30c5cc00dd67081d0031d4358131c48b1e" \
        "javert 0022eeb8632f0124df59701fcf76e394f1f50c83a5926044264639a46869fb2f" \
        "michael 3512b08d2594526394297c018ecf5cd78995adbae15160b71ce6f1cc2f75642a" ;;
    es-quality)
      printf '%s\n' \
        "lola 7e2fb321246bfbcb276caf24a84371253b6fb503a30d62c370d01eb4eefe6d5d" \
        "alba 89708034d6542dec62bf0aaf74284776c1cc06bdd7fcb6c64d983565a9cf04e8" \
        "eve 8bf37886f2cfe66fb450890c254148fa9d6183ad7bf30395be0e424eeabae954" \
        "fantine 47cff3324f69bd37a91e66691ca5831954ab422691b021d3af697228e453d864" \
        "giovanni 1035add0198abba837d05bad9efbb49673a89ad5fc588fb5204b6311517e1342" \
        "marius 4b11504d61aaffd0f1fbeeb6a322371b81dc0d0a60c405046132515b8006e9f6" \
        "javert db304428fda2bc9a0a3fdf65309e7775c7556a965a6ffe5e86239608c26f9c8c" \
        "michael e0090399e9df52d6ac48a67334a8d8e02239f2da03e63e46cad8c82986468b24" ;;
    en)
      printf '%s\n' \
        "alba d291428b416d6c36a1de7835e51dbe1e334b75e5af512bb18dd23a1047fe8f3b" \
        "eve ec2bf13d4ac4bcee200ee7aec1deae8132bb5bd60233f815e12a4d8efe56b8c6" \
        "jane fe0e73c3a01f9d3b344ce19c912654df1464cb4077393e188d54f6cff990c05f" \
        "mary 8eaed3701d6ba5602393b50876f6639d3ed5d7aacc356a3ee6337957a7cbe9d2" \
        "marius b05946f39371f8853902b0486231f2d401c8a9570da9978329120d4ecfd009e9" \
        "javert 580e4b72d4065e906c14e580f97da478c9707bead74b0ef9c049e0ca8de997d9" \
        "michael 401711f60394aa6085627f7050c1b3f97b31aa7138784811bc6c6ec7d7eaad0c" \
        "george 9cee0daccb0eda1631b73f8b35f6f8cb195885d55d2e1e20fc711f9e4456ceb0" ;;
  esac
}

pocket_graphs() {
  case "$1" in
    es-fast)
      printf '%s\n' \
        "bundle.json a45c4ffcdc1b6c9390630144f463c796e68bfdba4348db9892b2113d65ae0156" \
        "flow_lm_flow_int8.onnx 96c5840c4f6223bcb4e7ba3e4f1e44d6dfa4074ed8ecfd093ed7ce527426027c" \
        "flow_lm_main_int8.onnx 35183134a5b0529bbf6abe47c14865e5c31cfa8acbe63f63ea98926cee7700e1" \
        "mimi_decoder_int8.onnx 5617d22eacdf2b887cf92ddbe9758395a97d989d3059d31e8be54a8ddff4baa8" \
        "text_conditioner.onnx 3d6f3cc76051dc5809d8ed93286d1585a5f89b48681f58d5b9cc6753c787265e" \
        "tokenizer.json 9ade09bbdc64ab2b59952b90773fa0890b780ece4fd5aef0d36a00c6ad5f0531" ;;
    es-quality)
      printf '%s\n' \
        "bundle.json 75d988bc78536df7ab4e44021f9bad337460589ddbcbe502f169ae4657529f57" \
        "flow_lm_flow_int8.onnx 96c5840c4f6223bcb4e7ba3e4f1e44d6dfa4074ed8ecfd093ed7ce527426027c" \
        "flow_lm_main_int8.onnx 2b1d27f0a61575bcd09e104b35c868c1f4dae30ae9bc31ee17deb0930a50c7d6" \
        "mimi_decoder_int8.onnx 06aa8f2d35dffdee2594d7950d3b2f79b0503cce701950059e7b18f3d03c962b" \
        "text_conditioner.onnx 6bacc7830ef31411e7fd97934bd4ec730aafd55cf85d25e9244771b40d92f07f" \
        "tokenizer.json 9ade09bbdc64ab2b59952b90773fa0890b780ece4fd5aef0d36a00c6ad5f0531" ;;
    en)
      printf '%s\n' \
        "bundle.json 2ebe8a92417f147eed6b3fbe27fbaecf9192b023935f1314b1a405cec5a91257" \
        "flow_lm_flow_int8.onnx 6f1fb81ef14891fd85658d9492999859a18aca8f62104d11cbbfffc00ffe0887" \
        "flow_lm_main_int8.onnx abd8fb1e9b587fcabdd18939ece600176faca47b00a61099a2152f08aa5f67df" \
        "mimi_decoder_int8.onnx 0a27e17757b01cc6b0293d28d94b6bb12028dc5307d7ae38c35d5d2ade454bc1" \
        "text_conditioner.onnx b417ef3ce8d31b52309f2755104ffa4f1d5e0f081ace7c6a7539f3090af9ed25" \
        "tokenizer.json f498428e1eafee50492f7be13dc9bfafcfc12e508cd0eb1b01c92ecd5d8c6687" ;;
  esac
}

pocket_source() {
  case "$1" in
    es-fast)
      printf '%s\n' spanish \
        "$POCKET_REPO/1e08e6a23401048648a9fdcfde2f89348215c2a7/languages/spanish/model.safetensors" \
        c4988ff376692f2ab1e85b17350755b08f16e49c0b5f0de8b271efa9abd71de7 \
        "$POCKET_REPO/4e1e0a3e611c51c0b4ed8174fc10f32a54644303/languages/spanish/tokenizer.json" \
        9ade09bbdc64ab2b59952b90773fa0890b780ece4fd5aef0d36a00c6ad5f0531 \
        spanish ;;
    es-quality)
      printf '%s\n' spanish_24l \
        "$POCKET_REPO/4e1e0a3e611c51c0b4ed8174fc10f32a54644303/languages/spanish_24l/model.safetensors" \
        0e9dcb4dbd3da8a2bd3570e9bd3aeb4f3588164fe357fb19980472457863818f \
        "$POCKET_REPO/4e1e0a3e611c51c0b4ed8174fc10f32a54644303/languages/spanish_24l/tokenizer.json" \
        9ade09bbdc64ab2b59952b90773fa0890b780ece4fd5aef0d36a00c6ad5f0531 \
        spanish_24l ;;
    en)
      printf '%s\n' english \
        "$POCKET_REPO/e7205b6ee50e654a5ea19f0e9df2b0813b05e921/languages/english/model.safetensors" \
        916ccd2686e9311cb40054893a3c4284393d658825ffc714a276f3e9b152344f \
        "$POCKET_REPO/00eac05ed3d16bdc3f6b5d598874019c34a89214/languages/english/tokenizer.json" \
        f498428e1eafee50492f7be13dc9bfafcfc12e508cd0eb1b01c92ecd5d8c6687 \
        english ;;
  esac
}

pocket_graphs_present() {
  local folder="$POCKET_DIR/$1" name expected
  while read -r name expected; do
    [ -f "$folder/$name" ] || return 1
  done < <(pocket_graphs "$1")
}

pocket_graphs_match() {
  local folder="$1" variant="$2" name expected actual mismatched=0
  while read -r name expected; do
    actual="$(sha256_file "$folder/$name")"
    if [ "$actual" != "$expected" ]; then
      warn "Pocket $variant: $name differs from the reference export ($actual); its parity check passed."
      mismatched=1
    fi
  done < <(pocket_graphs "$variant")
  return "$mismatched"
}

pocket_toolchain() {
  local venv="$POCKET_WORK/venv"
  if [ -x "$venv/bin/python" ] && "$venv/bin/python" -c "import pocket_tts, onnxruntime, torch" >/dev/null 2>&1; then
    printf '%s\n' "$venv/bin/python"
    return 0
  fi
  if ! command -v uv >/dev/null 2>&1; then
    warn "uv is required to export the Pocket models (https://docs.astral.sh/uv/)."
    return 1
  fi
  mkdir -p "$POCKET_WORK"
  log "Preparing the Pocket export toolchain in $POCKET_WORK (CPU torch, one time, ~1 GB)..." >&2
  uv venv -q --python 3.12 "$venv" >&2 || return 1
  uv pip install -q --python "$venv/bin/python" --index-url https://download.pytorch.org/whl/cpu \
    "torch==2.14.1" >&2 || return 1
  uv pip install -q --python "$venv/bin/python" "$POCKET_SOURCE" "onnx==1.23.1" "onnxruntime==1.30.0" \
    "onnxscript==0.7.2" "numpy==2.5.3" >&2 || return 1
  printf '%s\n' "$venv/bin/python"
}

setup_pocket_voices() {
  local variant="$1" language name expected
  language="$(pocket_source "$variant" | sed -n 6p)"
  while read -r name expected; do
    fetch_pinned "$POCKET_REPO/$POCKET_VOICE_REVISION/languages/$language/embeddings/$name.safetensors" \
      "$POCKET_DIR/$variant/voices/$name.safetensors" "$expected" || return 1
  done < <(pocket_voices "$variant")
}

setup_pocket_variant() {
  local variant="$1"
  local source config weights_url weights_sha tokenizer_url tokenizer_sha
  mapfile -t source < <(pocket_source "$variant")
  config="${source[0]}"
  weights_url="${source[1]}"
  weights_sha="${source[2]}"
  tokenizer_url="${source[3]}"
  tokenizer_sha="${source[4]}"

  if pocket_graphs_present "$variant"; then
    log "Pocket $variant graphs already present."
  else
    local python
    python="$(pocket_toolchain)" || { warn "Pocket $variant not exported; Supertonic keeps answering."; return 0; }
    local inputs="$POCKET_WORK/inputs/$variant"
    log "Downloading the official Pocket $config weights (pinned)..."
    fetch_pinned "$weights_url" "$inputs/model.safetensors" "$weights_sha" || return 0
    fetch_pinned "$tokenizer_url" "$inputs/tokenizer.json" "$tokenizer_sha" || return 0
    local staging="$POCKET_DIR/.$variant.part"
    rm -rf "$staging"
    log "Exporting Pocket $variant to ONNX int8 (parity-checked against the official model)..."
    if ! "$python" "$TOOLS_DIR/export-pocket.py" --variant "$config" --weights "$inputs/model.safetensors" \
         --tokenizer "$inputs/tokenizer.json" --out "$staging" --threads "$(nproc 2>/dev/null || echo 4)"; then
      rm -rf "$staging"
      warn "Pocket $variant export failed; Supertonic keeps answering."
      return 0
    fi
    pocket_graphs_match "$staging" "$variant" || true
    if [ -d "$POCKET_DIR/$variant/voices" ]; then
      mv "$POCKET_DIR/$variant/voices" "$staging/voices"
    fi
    rm -rf "$POCKET_DIR/$variant"
    mv "$staging" "$POCKET_DIR/$variant"
    rm -f "$inputs/model.safetensors"
  fi

  if setup_pocket_voices "$variant"; then
    log "Pocket $variant ready."
  else
    warn "Pocket $variant voices incomplete; re-run provisioning."
  fi
}

write_pocket_notice() {
  cat > "$POCKET_DIR/NOTICE" <<'POCKET_NOTICE_EOF'
Pocket TTS — Kyutai

Model:      Pocket TTS (~100M params; Spanish 6-layer and 24-layer, English)
Authors:    Manu Orsini, Simon Rouard, Gabriel De Marmiesse, Václav Volhejn,
            Neil Zeghidour, Alexandre Défossez (Kyutai)
Weights:    https://huggingface.co/kyutai/pocket-tts-without-voice-cloning
            spanish      @1e08e6a23401048648a9fdcfde2f89348215c2a7
            spanish_24l  @4e1e0a3e611c51c0b4ed8174fc10f32a54644303
            english      @e7205b6ee50e654a5ea19f0e9df2b0813b05e921
            voices       @1e08e6a23401048648a9fdcfde2f89348215c2a7
Code:       https://github.com/kyutai-labs/pocket-tts (MIT)
License:    Creative Commons Attribution 4.0 International (CC BY 4.0)
            https://creativecommons.org/licenses/by/4.0/

Changes:    The weights were converted to ONNX and dynamically quantized to
            int8 (MatMul, per channel) by services/tts/tools/export-pocket.py;
            the predefined voice states are used unchanged.

Voices:     lola, giovanni (Common Voice, CC0); alba (Alba MacKenna, CC BY 4.0);
            eve, fantine, jane, mary, michael, george (VCTK, CC BY 4.0);
            marius, javert (Unmute voice donations, CC0).
            Voices built from non-commercial recordings (jean, cosette, ...)
            are deliberately not installed.

Kyutai's terms prohibit voice impersonation or cloning without explicit and
lawful consent, deception, and unlawful or harmful content. Provided "AS IS".
POCKET_NOTICE_EOF
}

setup_pocket_models() {
  if [ "${ARGUS_TTS_POCKET:-1}" = "0" ]; then
    log "Pocket TTS skipped (ARGUS_TTS_POCKET=0); Supertonic answers every language."
    return 0
  fi
  if ! command -v curl >/dev/null 2>&1; then
    warn "curl not found; skipping Pocket TTS."
    return 0
  fi
  log "Setting up Kyutai Pocket TTS (es fast ~170 MB, es quality ~545 MB, en ~175 MB)..."
  mkdir -p "$POCKET_DIR/references"
  local variants=(es-fast en)
  if [ "${ARGUS_TTS_POCKET_QUALITY:-1}" != "0" ]; then
    variants+=(es-quality)
  fi
  local variant
  for variant in "${variants[@]}"; do
    setup_pocket_variant "$variant"
  done
  write_pocket_notice
}

case "${1:-}" in
  --help|-h)
    cat <<'USAGE_EOF'
Usage: provision.sh

Provisions the argus-tts models under models/tts:
  Supertonic 3 (onnx/, voice_styles/), downloaded when missing.
  Kyutai Pocket TTS (pocket/es-fast, pocket/es-quality, pocket/en): the
  official CC BY 4.0 weights are downloaded with SHA-256 pins, exported to
  ONNX int8 by tools/export-pocket.py in a cached uv toolchain, checked for
  parity against the official model, and moved into place atomically; the
  predefined voices are downloaded with SHA-256 pins. Present files are kept.

Environment:
  ARGUS_TTS_POCKET=0           skip Pocket entirely (Supertonic answers)
  ARGUS_TTS_POCKET_QUALITY=0   skip the 24-layer Spanish model (~545 MB)
  ARGUS_POCKET_WORK=<dir>      export toolchain cache (default ~/.cache/argus/pocket-export)
USAGE_EOF
    exit 0
    ;;
esac

setup_tts_model
setup_pocket_models
