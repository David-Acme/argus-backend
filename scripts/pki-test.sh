#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKI_LIB="${ARGUS_PKI_LIB:-$ROOT/scripts/lib/pki.sh}"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

fail() {
  printf 'pki-test: %s\n' "$*" >&2
  exit 1
}

issue() {
  local case_dir="$1"
  local mdns_name="$2"
  mkdir -p "$case_dir"
  printf '[mdns]\nname = "%s"\n' "$mdns_name" > "$case_dir/config.toml"
  bash -c '
    set -euo pipefail
    source "$0/scripts/lib/common.sh"
    source "$1"
    ensure_instance_certs "$2" "$2/certs" "$2/config.toml" "$2/ca" "192.168.1.20"
  ' "$ROOT" "$PKI_LIB" "$case_dir" > "$case_dir/output.log" 2>&1 ||
    fail "ensure_instance_certs failed for mdns.name '$mdns_name'"
}

index=0
for mdns_name in "casa-argus" "Argus" "Argus_Home" "Mi Argus" "argus.local"; do
  index=$((index + 1))
  case_dir="$TEST_TMP/case-$index"
  issue "$case_dir" "$mdns_name"
  openssl verify -x509_strict -CAfile "$case_dir/certs/ca.pem" \
    "$case_dir/certs/server.pem" > /dev/null 2>&1 ||
    fail "the leaf for mdns.name '$mdns_name' violates its own CA's name constraints"
  code="$(tr -d '[:space:]' < "$case_dir/ca/pairing.code")"
  printf '%s' "$code" | grep -Eq '^[A-Z2-7]{26}$' ||
    fail "the pairing code is not 26 base32 characters"
  if grep -Fq "$code" "$case_dir/output.log"; then
    fail "the pairing code was printed to the console"
  fi
done

san="$(openssl x509 -in "$TEST_TMP/case-1/certs/server.pem" -noout -ext subjectAltName 2>/dev/null || true)"
printf '%s' "$san" | grep -Fq 'DNS:casa-argus' ||
  fail "a valid mdns.name is missing from the leaf SAN"

printf 'pki-test: ok\n'
