#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/tunnel-pki-test.sh

Issues the device-name certificates of scripts/lib/tunnel-pki.sh in a scratch
directory and checks them: an instance CA made with this home's ten device
names permitted, one 2-year certificate per service whose one SAN is
<service>--<home-id>.argus-tunnel.lynk.page, renewal under 60 days, a key that
no longer matches, a CA whose constraints leave the names out (refused, nothing
written), a CA without constraints, no home id, the native config keys, a TLS
handshake that checks the dialled name, and a set -x trace that carries no
private key.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
server_pid=""
cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$TEST_TMP"
}
trap cleanup EXIT

source "$ROOT/scripts/lib/common.sh"
source "$ROOT/scripts/lib/pki.sh"
source "$ROOT/scripts/lib/internal-pki.sh"
source "$ROOT/scripts/lib/tunnel-pki.sh"

fail() {
  printf 'tunnel-pki-test: %s\n' "$*" >&2
  exit 1
}

HOME_ID="0123456789abcdef0123456789abcdef"
OTHER_HOME_ID="fedcba9876543210fedcba9876543210"

tunnel_config() {
  local path="$1"
  local home_id="$2"
  mkdir -p "$(dirname "$path")"
  printf '[link]\nhome_id = "%s"\n' "$home_id" > "$path"
}

fingerprint() {
  openssl x509 -in "$1" -noout -fingerprint -sha256
}

days_valid() {
  local start end
  start="$(date -d "$(openssl x509 -in "$1" -noout -startdate | sed 's/^notBefore=//')" +%s)"
  end="$(date -d "$(openssl x509 -in "$1" -noout -enddate | sed 's/^notAfter=//')" +%s)"
  printf '%s\n' "$(( (end - start) / 86400 ))"
}

mode_of() {
  stat -c '%a' "$1"
}

state_of() {
  find "$1" -type f | sort | xargs sha256sum
}

home="$TEST_TMP/home"
config="$home/services/tunnel/config.toml"
tunnel_config "$config" "$HOME_ID"

[ "$(tunnel_pki_name sync "$HOME_ID")" = "sync--$HOME_ID.argus-tunnel.lynk.page" ] ||
  fail "a device name is <service>--<home-id>.argus-tunnel.lynk.page"
names="$(tunnel_pki_ca_names "$config")"
[ "$(wc -w <<<"$names")" -eq 10 ] || fail "the CA names are the ten services, got: $names"
for service in auth identity sync camera productivity notification guard settings rtc turn; do
  grep -qw "$service--$HOME_ID.argus-tunnel.lynk.page" <<<"$names" ||
    fail "the CA names leave out $service"
done
[ -z "$(tunnel_pki_ca_names "$TEST_TMP/absent.toml")" ] || fail "a missing tunnel config names nothing"
printf '[link]\nhome_id = "not-a-home-id"\n' > "$TEST_TMP/bad.toml"
[ -z "$(tunnel_pki_ca_names "$TEST_TMP/bad.toml")" ] || fail "a malformed home id names nothing"

certs="$home/certs"
ARGUS_CA_EXTRA_DNS="$names" ensure_instance_certs "$home" "$certs" "" "$home/ca" > /dev/null
openssl x509 -in "$certs/ca.pem" -noout -ext nameConstraints > "$TEST_TMP/constraints.txt"
grep -q "DNS:sync--$HOME_ID.argus-tunnel.lynk.page" "$TEST_TMP/constraints.txt" ||
  fail "a new instance CA does not permit this home's device names"
if grep -q "DNS:argus-tunnel.lynk.page" "$TEST_TMP/constraints.txt"; then
  fail "a new instance CA permits the whole tunnel domain instead of this home's names"
fi

pki="$certs/tunnel"
ensure_tunnel_pki "$pki" "$certs/ca.pem" "$home/ca/ca.key" "$config" > "$TEST_TMP/first.log"
[ "$(mode_of "$pki")" = 700 ] || fail "the tunnel certificate directory is not 0700"
for service in auth identity sync camera productivity notification guard settings rtc turn; do
  name="$service--$HOME_ID.argus-tunnel.lynk.page"
  leaf="$pki/$service"
  [ -s "$leaf/cert.pem" ] && [ -s "$leaf/key.pem" ] || fail "$service has no device-name certificate"
  [ "$(mode_of "$leaf")" = 700 ] || fail "$service's directory is not 0700"
  [ "$(mode_of "$leaf/key.pem")" = 600 ] || fail "$service's key is not 0600"
  [ "$(tunnel_pki_san "$leaf/cert.pem")" = "DNS:$name" ] ||
    fail "$service's certificate names more or less than $name: $(tunnel_pki_san "$leaf/cert.pem")"
  [ "$(internal_pki_subject "$leaf/cert.pem")" = "CN=Argus tunnel $service,OU=tunnel,O=Argus" ] ||
    fail "$service's subject is not the tunnel subject: $(internal_pki_subject "$leaf/cert.pem")"
  [ "$(days_valid "$leaf/cert.pem")" -eq 730 ] || fail "$service's certificate is not valid for 730 days"
  openssl verify -CAfile "$certs/ca.pem" -purpose sslserver "$leaf/cert.pem" > /dev/null ||
    fail "$service's certificate does not verify against the instance CA"
  openssl x509 -in "$leaf/cert.pem" -noout -ext extendedKeyUsage | grep -q "TLS Web Server Authentication" ||
    fail "$service's certificate is not a server certificate"
  if openssl x509 -in "$leaf/cert.pem" -noout -ext extendedKeyUsage | grep -q "Client Authentication"; then
    fail "$service's certificate may also act as a client"
  fi
  [ "$(grep -c 'BEGIN CERTIFICATE' "$leaf/cert.pem")" -eq 2 ] ||
    fail "$service's certificate file is not the leaf followed by the CA"
  internal_pki_pair_matches "$leaf/cert.pem" "$leaf/key.pem" || fail "$service's key does not match"
done
grep -q "issued 730-day device-name certificates for auth identity sync camera productivity notification guard settings rtc turn" \
  "$TEST_TMP/first.log" || fail "the first run does not report the ten certificates"

before="$(state_of "$pki")"
ensure_tunnel_pki "$pki" "$certs/ca.pem" "$home/ca/ca.key" "$config" > "$TEST_TMP/second.log"
[ "$(state_of "$pki")" = "$before" ] || fail "a second run re-issued certificates that were current"
grep -q "every device-name certificate is valid for more than 60 days" "$TEST_TMP/second.log" ||
  fail "a second run does not say every certificate is current"

old_sync="$(fingerprint "$pki/sync/cert.pem")"
old_camera="$(fingerprint "$pki/camera/cert.pem")"
tunnel_pki_issue_leaf "$pki/sync" "sync--$HOME_ID.argus-tunnel.lynk.page" "$certs/ca.pem" "$home/ca/ca.key" 30
short_sync="$(fingerprint "$pki/sync/cert.pem")"
openssl ecparam -name prime256v1 -genkey -noout -out "$pki/camera/key.pem" 2> /dev/null
ensure_tunnel_pki "$pki" "$certs/ca.pem" "$home/ca/ca.key" "$config" > "$TEST_TMP/renew.log"
[ "$(fingerprint "$pki/sync/cert.pem")" != "$short_sync" ] || fail "a certificate under 60 days was not renewed"
[ "$(days_valid "$pki/sync/cert.pem")" -eq 730 ] || fail "the renewed certificate is not valid for 730 days"
[ "$(fingerprint "$pki/camera/cert.pem")" != "$old_camera" ] || fail "a certificate whose key changed was not re-issued"
internal_pki_pair_matches "$pki/camera/cert.pem" "$pki/camera/key.pem" || fail "the re-issued certificate does not match its key"
[ "$(fingerprint "$pki/sync/cert.pem")" != "$old_sync" ] || fail "the renewal kept the old certificate"
grep -q "issued 730-day device-name certificates for sync camera" "$TEST_TMP/renew.log" ||
  fail "the renewal does not name what it re-issued"

port_free() {
  ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

port=""
for _ in $(seq 1 50); do
  candidate=$(( 20000 + RANDOM % 40000 ))
  case "$candidate" in 4222|9000|9001) continue ;; esac
  if port_free "$candidate"; then
    port="$candidate"
    break
  fi
done
[ -n "$port" ] || fail "no free loopback port"

openssl s_server -accept "127.0.0.1:$port" -naccept 3 -quiet \
  -cert "$pki/sync/cert.pem" -key "$pki/sync/key.pem" > "$TEST_TMP/server.out" 2>&1 &
server_pid=$!
listening=0
for _ in $(seq 1 100); do
  if ! port_free "$port"; then
    listening=1
    break
  fi
  sleep 0.05
done
[ "$listening" -eq 1 ] || fail "openssl s_server did not listen"
openssl s_client -connect "127.0.0.1:$port" -servername "sync--$HOME_ID.argus-tunnel.lynk.page" \
  -CAfile "$certs/ca.pem" -verify_return_error -verify_hostname "sync--$HOME_ID.argus-tunnel.lynk.page" \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "an app dialling sync's device name refused the handshake: $(tail -n 3 "$TEST_TMP/client.out")"
grep -q 'Verify return code: 0 (ok)' "$TEST_TMP/client.out" || fail "an app could not verify sync's device name"
! openssl s_client -connect "127.0.0.1:$port" -servername "auth--$HOME_ID.argus-tunnel.lynk.page" \
  -CAfile "$certs/ca.pem" -verify_return_error -verify_hostname "auth--$HOME_ID.argus-tunnel.lynk.page" \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "sync's certificate passed for auth's device name"
grep -qi "hostname mismatch" "$TEST_TMP/client.out" ||
  fail "the refusal for auth's device name was not a name mismatch: $(tail -n 3 "$TEST_TMP/client.out")"
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=""

narrow="$TEST_TMP/narrow"
ensure_instance_certs "$narrow" "$narrow/certs" "" "$narrow/ca" > /dev/null
ensure_tunnel_pki "$narrow/certs/tunnel" "$narrow/certs/ca.pem" "$narrow/ca/ca.key" "$config" > "$TEST_TMP/narrow.log"
grep -q "does not permit the device names of auth identity sync camera productivity notification guard settings rtc turn" \
  "$TEST_TMP/narrow.log" || fail "a CA whose constraints leave the names out was not reported"
[ -z "$(find "$narrow/certs/tunnel" -type f)" ] || fail "a refused certificate was left on disk"

legacy="$TEST_TMP/legacy"
mkdir -p "$legacy"
openssl ecparam -name prime256v1 -genkey -noout -out "$legacy/ca.key" 2> /dev/null
openssl req -x509 -new -key "$legacy/ca.key" -sha256 -days 3650 -subj "/CN=Argus Instance CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out "$legacy/ca.pem" 2> /dev/null
ensure_tunnel_pki "$legacy/tunnel" "$legacy/ca.pem" "$legacy/ca.key" "$config" > /dev/null
openssl verify -CAfile "$legacy/ca.pem" -purpose sslserver "$legacy/tunnel/guard/cert.pem" > /dev/null ||
  fail "a CA made before name constraints cannot sign the device names"

moved="$TEST_TMP/moved.toml"
tunnel_config "$moved" "$OTHER_HOME_ID"
ensure_tunnel_pki "$legacy/tunnel" "$legacy/ca.pem" "$legacy/ca.key" "$moved" > /dev/null
[ "$(tunnel_pki_san "$legacy/tunnel/guard/cert.pem")" = "DNS:guard--$OTHER_HOME_ID.argus-tunnel.lynk.page" ] ||
  fail "a new home id did not re-issue the device names"

unnamed="$TEST_TMP/unnamed"
printf '[link]\nhome_id = ""\n' > "$TEST_TMP/empty.toml"
ensure_tunnel_pki "$unnamed" "$certs/ca.pem" "$home/ca/ca.key" "$TEST_TMP/empty.toml" > "$TEST_TMP/unnamed.log"
[ ! -e "$unnamed" ] || fail "a tunnel without a home id issued certificates"
grep -q "names no home id yet" "$TEST_TMP/unnamed.log" || fail "a tunnel without a home id was not reported"

for service in sync guard; do
  mkdir -p "$home/services/$service"
  printf '[remote]\ntunnel_port = 0\ntunnel_cert = ""\ntunnel_key = ""\n' > "$home/services/$service/config.toml"
done
tunnel_pki_configure_native "$home" "$pki"
[ "$(toml_value "$home/services/sync/config.toml" remote tunnel_cert)" = "$pki/sync/cert.pem" ] ||
  fail "the native sync config does not name its device-name certificate"
[ "$(toml_value "$home/services/guard/config.toml" remote tunnel_key)" = "$pki/guard/key.pem" ] ||
  fail "the native guard config does not name its device-name key"
[ "$(toml_value "$home/services/sync/config.toml" remote tunnel_port)" = "0" ] ||
  fail "writing the pair changed the tunnel port"

traced="$TEST_TMP/traced"
(
  set -x
  ensure_tunnel_pki "$traced" "$certs/ca.pem" "$home/ca/ca.key" "$config"
) > "$TEST_TMP/trace.log" 2>&1
[ -s "$traced/settings/key.pem" ] || fail "the traced run issued nothing"
if grep -q "PRIVATE KEY" "$TEST_TMP/trace.log"; then
  fail "a set -x trace of the issuance carries private key material"
fi
key_body="$(sed -n '2p' "$traced/settings/key.pem")"
if grep -qF "$key_body" "$TEST_TMP/trace.log"; then
  fail "a set -x trace of the issuance carries a private key line"
fi

echo "tunnel-pki-test: passed"
