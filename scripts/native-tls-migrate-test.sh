#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/native-tls-migrate-test.sh

Checks the native dial-target migration of scripts/lib/internal-pki.sh on a
fixture. The fixture is a native tree built from the native templates, with the
dial keys of the list set back to their pre-change plaintext loopback values.
Case a issues the internal PKI in a scratch directory and wires it into the
configs as setup.sh does: every listed key must move to tls: on its callee's
mutual TLS port, one line must be printed per changed key, a second run must
change nothing, and no other line of any config may change. Case b has no
certificate: the configs must stay as they were and the warning must name
scripts/setup.sh. The native templates must carry the same targets as the list.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v openssl >/dev/null 2>&1; then
  printf 'SKIPPED: native-tls-migrate-test needs openssl\n'
  exit 77
fi
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

source "$ROOT/scripts/lib/common.sh"
source "$ROOT/scripts/lib/internal-pki.sh"

fail() {
  printf 'native-tls-migrate-test: %s\n' "$*" >&2
  exit 1
}

rows="$TEST_TMP/rows"
internal_pki_native_dial_rows > "$rows"
row_count="$(wc -l < "$rows")"

make_tree() {
  local root="$1"
  local service
  for service in $INTERNAL_PKI_SERVICES; do
    mkdir -p "$root/services/$service"
    cp "$ROOT/services/$service/config.toml.example" "$root/services/$service/config.toml"
  done
}

plaintext_tree() {
  local root="$1"
  local dialer table key callee tls_port
  make_tree "$root"
  while read -r dialer table key callee; do
    tls_port="$(internal_pki_tls_port_of "$root" "$callee")" ||
      fail "no TLS port for $callee in the fixture"
    replace_toml_value "$table" "$key" "127.0.0.1:$((tls_port - 200))" \
      "$root/services/$dialer/config.toml"
  done < "$rows"
}

expected_changes() {
  local service="$1"
  awk -v s="$service" '$1 == s' "$rows" | wc -l
}

changed_lines() {
  local before="$1" after="$2" service="$3"
  diff "$before/services/$service/config.toml" "$after/services/$service/config.toml" |
    grep -c '^<' || true
}

case_a="$TEST_TMP/a"
plaintext_tree "$case_a"
ensure_internal_pki "$TEST_TMP/pki" > /dev/null
internal_pki_configure_native "$case_a" "$TEST_TMP/pki"
cp -r "$case_a" "$TEST_TMP/a-before"

migrate_native_dial_targets "$case_a" > "$TEST_TMP/a.out" 2>&1 ||
  fail "case a: the migration failed"
printed="$(grep -c ' -> tls:127.0.0.1:' "$TEST_TMP/a.out" || true)"
[ "$printed" -eq "$row_count" ] ||
  fail "case a: $printed lines for $row_count keys, want one line per changed key"

while read -r dialer table key callee; do
  want="tls:127.0.0.1:$(internal_pki_tls_port_of "$case_a" "$callee")"
  got="$(toml_value "$case_a/services/$dialer/config.toml" "$table" "$key")"
  [ "$got" = "$want" ] || fail "case a: $dialer [$table] $key is '$got', want '$want'"
done < "$rows"

for service in $INTERNAL_PKI_SERVICES; do
  [ "$(changed_lines "$TEST_TMP/a-before" "$case_a" "$service")" -eq "$(expected_changes "$service")" ] ||
    fail "case a: $service has changed lines beyond its listed keys"
done

migrate_native_dial_targets "$case_a" > "$TEST_TMP/a2.out" 2>&1
[ "$(grep -c ' -> tls:' "$TEST_TMP/a2.out" || true)" -eq 0 ] ||
  fail "case a: a second run changed keys again"

case_b="$TEST_TMP/b"
plaintext_tree "$case_b"
cp -r "$case_b" "$TEST_TMP/b-before"

migrate_native_dial_targets "$case_b" > "$TEST_TMP/b.out" 2>&1 ||
  fail "case b: the migration failed without a certificate"
[ "$(grep -c ' -> tls:' "$TEST_TMP/b.out" || true)" -eq 0 ] ||
  fail "case b: a key moved to tls: without a certificate"
diff -r "$TEST_TMP/b-before" "$case_b" > /dev/null ||
  fail "case b: a config changed without a certificate"
grep -q 'scripts/setup.sh' "$TEST_TMP/b.out" ||
  fail "case b: the warning does not name scripts/setup.sh"

while read -r dialer table key callee; do
  case "$table" in
    owners.*) continue ;;
  esac
  want="tls:127.0.0.1:$(internal_pki_tls_port_of "$ROOT" "$callee")"
  got="$(toml_value "$ROOT/services/$dialer/config.toml.example" "$table" "$key")"
  [ "$got" = "$want" ] ||
    fail "template: $dialer [$table] $key is '$got', the list says '$want'"
done < "$rows"

printf 'native-tls-migrate-test: %s keys migrated on the fixture, a second run is a no-op, no certificate leaves the configs alone, and the templates match the list\n' "$row_count"
