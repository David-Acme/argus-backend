#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/tmp"
export TMPDIR="$work/tmp"

source_config="$work/source.toml"
config="$work/stack.toml"

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

[ -z "$(ls -A "$work/tmp")" ] || fail "a copy of the stack config was left in TMPDIR"
[ -z "$(trap -p RETURN)" ] || fail "install_source_config left a RETURN trap behind"

printf 'native-stack-prepare-test: ok\n'
