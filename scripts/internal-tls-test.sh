#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/internal-tls-test.sh <test binary> [<minimum test cases>]

Issues a scratch internal CA and its service certificates with
scripts/lib/internal-pki.sh, adds a foreign client certificate the internal CA
did not issue, and runs the given test binary against them, with the PKI in
ARGUS_INTERNAL_TLS_PKI and the foreign certificate in ARGUS_INTERNAL_TLS_FOREIGN.
It fails when the binary lists fewer test cases than the minimum, 25 by
default, which is lib/grpc's internal-tls-test; a service's test of its own
mutual TLS listener names its own count. In lib/grpc's test two services speak
mutual TLS through argus::client::makeChannel, the client checks the server's
name, the server refuses a foreign client, a client without a certificate and a
plaintext client, installInternalTls refuses a second install, an install
after a channel and empty material, and installInternalTlsFrom installs a
service directory, installs nothing for an empty setting and throws for a
directory without the material, and listenPorts and startListening open a
mutual TLS port beside the plaintext one only with material and an address,
serve a peer of the internal CA there and refuse every other client, start
nothing when a port cannot be bound, and refuse a message over the cap on
both ports while serving one under it, and listenFailure names the ports
that were asked for and the consequence only when nothing came up. A process
installs its certificate once, so every test case runs in a process of its
own. The case list is taken with --no-skip, so a case decorated
doctest::skip is still listed and counts toward the minimum, and a case whose
own process reports exactly one skipped test case is accepted - its output is
passed through so the reason it printed reaches the runner and nothing fails.
A case that neither reports one passing test case nor one skipped one does
fail. Without openssl the test exits 77, which ctest reports as skipped.
USAGE
}

[ "$#" -eq 1 ] || [ "$#" -eq 2 ] || { usage >&2; exit 2; }
test_bin="$1"
minimum="${2:-25}"
case "$minimum" in ""|*[!0-9]*) usage >&2; exit 2 ;; esac
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

"$test_bin" --no-skip --list-test-cases > "$work/list.out" 2>&1 \
  || { cat "$work/list.out" >&2; printf 'internal-tls-test: the test cases could not be listed\n' >&2; exit 1; }
awk '
  /listing all test case names/ { listing = 1; next }
  listing && /^=+$/ { if (started) exit; started = 1; next }
  started && NF { print }
' "$work/list.out" > "$work/cases"

ran=0
failed=0
skipped=0
while IFS= read -r name; do
  filter="${name//\\/\\\\}"
  filter="${filter//,/\\,}"
  if ! ARGUS_INTERNAL_TLS_PKI="$work/internal" ARGUS_INTERNAL_TLS_FOREIGN="$work/foreign" \
      "$test_bin" --test-case="$filter" < /dev/null > "$work/case.out" 2>&1; then
    cat "$work/case.out" >&2
    printf 'internal-tls-test: failed: %s\n' "$name" >&2
    failed=$((failed + 1))
  elif grep -Eq 'test cases: *0 \| *0 passed \| *0 failed \| *[1-9][0-9]* skipped' "$work/case.out"; then
    cat "$work/case.out"
    skipped=$((skipped + 1))
  elif ! grep -Eq 'test cases: *1 \| *1 passed' "$work/case.out"; then
    cat "$work/case.out" >&2
    printf 'internal-tls-test: did not run exactly one case: %s\n' "$name" >&2
    failed=$((failed + 1))
  fi
  ran=$((ran + 1))
done < "$work/cases"

[ "$ran" -ge "$minimum" ] || { printf 'internal-tls-test: only %s test cases were found\n' "$ran" >&2; exit 1; }
[ "$failed" -eq 0 ] || { printf 'internal-tls-test: %s of %s test cases failed\n' "$failed" "$ran" >&2; exit 1; }
if [ "$skipped" -eq 0 ]; then
  printf 'internal-tls-test: %s test cases passed, each in a process of its own\n' "$ran"
else
  printf 'internal-tls-test: %s test cases passed and %s skipped with their reason above, each in a process of its own\n' "$((ran - skipped))" "$skipped"
fi
