#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

source_config="$work/source.toml"
config="$work/stack.toml"
code="$work/pairing.code"

cat > "$source_config" <<'EOF'
[pairing]
paired = false
owner_device = ""

[push]
enabled = false
EOF

fail() {
  printf 'native-stack-prepare-test: %s\n' "$*" >&2
  exit 1
}

expect() {
  local got
  got="$(toml_value "$config" "$1" "$2")"
  [ "$got" = "$3" ] || fail "[$1] $2 is '$got', expected '$3'"
}

install_source_config "$source_config" "$config"
expect pairing paired false
expect pairing owner_device ""

printf 'ABCDEFGHJKLMNPQRSTUVWXYZ23\n' > "$code"
code_before="$(sha256sum "$code" | cut -d' ' -f1)"
replace_toml_value pairing paired true "$config" literal
replace_toml_value pairing owner_device "device-hash-1" "$config"
replace_toml_value push enabled true "$config" literal

install_source_config "$source_config" "$config"
expect pairing paired true
expect pairing owner_device device-hash-1
expect push enabled true

install_source_config "$source_config" "$config"
expect pairing paired true
expect pairing owner_device device-hash-1
expect push enabled true

[ "$(sha256sum "$code" | cut -d' ' -f1)" = "$code_before" ] || fail "the code file changed"
[ ! -e "$config.previous" ] || fail "the previous copy was left behind"

printf 'native-stack-prepare-test: ok\n'
