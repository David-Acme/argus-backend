#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/internal-tls-test.sh <internal-tls-test>

Issues a scratch internal CA and its service certificates with
scripts/lib/internal-pki.sh, adds a foreign client certificate the internal CA
did not issue, and runs the given test binary against them: two services speak
mutual TLS through argus::client::makeChannel, the client checks the server's
name, and the server refuses a foreign client, a client without a certificate
and a plaintext client. Without openssl the test exits 77, which ctest reports
as skipped.
USAGE
}

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
test_bin="$1"
if ! command -v openssl >/dev/null 2>&1; then
  printf 'internal-tls-test: skipped, no openssl\n'
  exit 77
fi
[ -x "$test_bin" ] || { printf 'internal-tls-test: %s is not executable\n' "$test_bin" >&2; exit 2; }

source "$ROOT/scripts/lib/common.sh"
source "$ROOT/scripts/lib/internal-pki.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

ensure_internal_pki "$work/internal" > "$work/pki.out"
mkdir -p "$work/foreign"
chmod 700 "$work/foreign"
(umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$work/foreign/key.pem")
openssl req -x509 -new -key "$work/foreign/key.pem" -sha256 -days 30 \
  -subj "/O=Argus/OU=$INTERNAL_PKI_PROFILE/CN=argus-camera" \
  -addext "subjectAltName=DNS:argus-camera,DNS:localhost,IP:127.0.0.1" \
  -addext "extendedKeyUsage=serverAuth,clientAuth" \
  -out "$work/foreign/cert.pem"
cp "$work/internal/camera/ca.pem" "$work/foreign/ca.pem"

ARGUS_INTERNAL_TLS_PKI="$work/internal" \
ARGUS_INTERNAL_TLS_FOREIGN="$work/foreign" \
  "$test_bin"
