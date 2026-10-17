#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/internal-pki-test.sh

Issues the internal CA and the service certificates of scripts/lib/internal-pki.sh
in a scratch directory and checks them: the CA (10 years, the only issuer), one
2-year certificate per internal gRPC service with server and client use, renewal
under 60 days, a key that no longer matches, an older profile and a CA rotation,
the trust bundle each service gets, mutual TLS between two services with
openssl, and a set -x trace that carries no private key.
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
source "$ROOT/scripts/lib/internal-pki.sh"

fail() {
  printf 'internal-pki-test: %s\n' "$*" >&2
  exit 1
}

pki="$TEST_TMP/certs/internal"

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
  find "$pki" -type f | sort | xargs sha256sum
}

grpc_services_in_templates() {
  local template name
  for template in "$ROOT"/argus-deploy/config.*.toml.example; do
    name="${template##*/config.}"
    name="${name%.toml.example}"
    if awk '
      /^\[internal_tls\]/ { tls = 1 }
      /^\[rpc\.callers\]/ { found = 1 }
      /^caller_[a-z]+[[:space:]]*=/ { found = 1 }
      END { exit !(tls && found) }' "$template"; then
      printf '%s\n' "$name"
    fi
  done | sort
}

[ "$(grpc_services_in_templates | tr '\n' ' ')" = "$(printf '%s\n' $INTERNAL_PKI_SERVICES | sort | tr '\n' ' ')" ] \
  || fail "the internal certificate list ($INTERNAL_PKI_SERVICES) is not the set of services that speak internal gRPC ($(grpc_services_in_templates | tr '\n' ' '))"

ensure_internal_pki "$pki" > "$TEST_TMP/first.out" 2>&1 || fail "the first issuance failed: $(cat "$TEST_TMP/first.out")"

[ "$(mode_of "$pki")" = 700 ] || fail "the internal PKI directory is not 0700"
[ "$(mode_of "$pki/ca.key")" = 600 ] || fail "the CA key is not 0600"
[ "$(mode_of "$pki/ca.pem")" = 644 ] || fail "the CA certificate is not 0644"
openssl x509 -in "$pki/ca.pem" -noout -ext basicConstraints | grep -q 'CA:TRUE, pathlen:0' \
  || fail "the internal CA is not a CA limited to one level"
openssl x509 -in "$pki/ca.pem" -noout -ext keyUsage | grep -q 'Certificate Sign, CRL Sign' \
  || fail "the internal CA cannot sign certificates"
[ "$(days_valid "$pki/ca.pem")" = 3650 ] || fail "the internal CA is not valid for 10 years"
case "$(internal_pki_subject "$pki/ca.pem")" in
  "CN=Argus internal CA "*",OU=internal-v1,O=Argus") ;;
  *) fail "the internal CA subject is $(internal_pki_subject "$pki/ca.pem")" ;;
esac
openssl verify -CAfile "$pki/ca.pem" "$pki/ca.pem" >/dev/null 2>&1 || fail "the internal CA is not self-signed"
cmp -s "$pki/ca.pem" "$pki/trust.pem" || fail "a fresh trust bundle is not the CA alone"

for service in $INTERNAL_PKI_SERVICES; do
  leaf="$pki/$service"
  [ "$(mode_of "$leaf")" = 700 ] || fail "the directory of $service is not 0700"
  [ "$(mode_of "$leaf/key.pem")" = 600 ] || fail "the key of $service is not 0600"
  [ "$(mode_of "$leaf/cert.pem")" = 644 ] || fail "the certificate of $service is not 0644"
  [ "$(find "$leaf" -type f | sort | tr '\n' ' ')" = "$leaf/ca.pem $leaf/cert.pem $leaf/key.pem " ] \
    || fail "the directory of $service holds more than its certificate, its key and the trust bundle"
  cmp -s "$leaf/ca.pem" "$pki/trust.pem" || fail "$service does not carry the trust bundle"
  openssl verify -CAfile "$pki/trust.pem" "$leaf/cert.pem" >/dev/null 2>&1 \
    || fail "the certificate of $service does not verify against the internal CA"
  internal_pki_pair_matches "$leaf/cert.pem" "$leaf/key.pem" || fail "the key of $service does not match its certificate"
  [ "$(days_valid "$leaf/cert.pem")" = 730 ] || fail "the certificate of $service is not valid for 2 years"
  [ "$(internal_pki_subject "$leaf/cert.pem")" = "CN=argus-$service,OU=internal-v1,O=Argus" ] \
    || fail "the certificate of $service has the subject $(internal_pki_subject "$leaf/cert.pem")"
  sans="$(openssl x509 -in "$leaf/cert.pem" -noout -ext subjectAltName)"
  for name in "DNS:argus-$service" "DNS:localhost" "IP Address:127.0.0.1" "IP Address:0:0:0:0:0:0:0:1"; do
    grep -qF "$name" <<<"$sans" || fail "the certificate of $service does not name $name"
  done
  openssl x509 -in "$leaf/cert.pem" -noout -ext extendedKeyUsage \
    | grep -q 'TLS Web Server Authentication, TLS Web Client Authentication' \
    || fail "the certificate of $service is not for both server and client use"
  openssl x509 -in "$leaf/cert.pem" -noout -ext basicConstraints | grep -q 'CA:FALSE' \
    || fail "the certificate of $service can sign certificates"
done

first_state="$(state_of)"
ensure_internal_pki "$pki" > "$TEST_TMP/second.out" 2>&1 || fail "the second run failed"
[ "$(state_of)" = "$first_state" ] || fail "a second run changed a certificate or a key"
grep -q 'valid for more than 60 days' "$TEST_TMP/second.out" || fail "a second run did not say every certificate is valid"

kept="$(fingerprint "$pki/auth/cert.pem")"
internal_pki_issue_leaf "$pki" camera 59
internal_pki_issue_leaf "$pki" guard 61
guard_before="$(fingerprint "$pki/guard/cert.pem")"
ensure_internal_pki "$pki" > "$TEST_TMP/renew.out" 2>&1 || fail "the renewal run failed"
[ "$(days_valid "$pki/camera/cert.pem")" = 730 ] || fail "a certificate with 59 days left was not renewed"
[ "$(fingerprint "$pki/guard/cert.pem")" = "$guard_before" ] || fail "a certificate with 61 days left was renewed"
[ "$(fingerprint "$pki/auth/cert.pem")" = "$kept" ] || fail "a renewal touched a certificate that did not need it"
grep -q 'certificates for camera$' "$TEST_TMP/renew.out" || fail "the renewal did not name the one certificate it issued"

(umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$pki/llm/key.pem")
ensure_internal_pki "$pki" > /dev/null 2>&1 || fail "the run after a key change failed"
internal_pki_pair_matches "$pki/llm/cert.pem" "$pki/llm/key.pem" || fail "a key that no longer matched was not re-issued"

openssl req -new -key "$pki/sync/key.pem" -subj "/O=Argus/OU=internal-v0/CN=argus-sync" -out "$TEST_TMP/old.csr"
openssl x509 -req -in "$TEST_TMP/old.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -set_serial 7 -days 700 \
  -extfile <(internal_pki_leaf_extensions sync) -out "$pki/sync/cert.pem" 2>/dev/null
ensure_internal_pki "$pki" > /dev/null 2>&1 || fail "the run after a profile change failed"
[ "$(internal_pki_subject "$pki/sync/cert.pem")" = "CN=argus-sync,OU=internal-v1,O=Argus" ] \
  || fail "a certificate of an older profile was not re-issued"

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

serve_from() {
  openssl s_server -accept "127.0.0.1:$port" -naccept 2 -quiet \
    -cert "$1/cert.pem" -key "$1/key.pem" -CAfile "$2" \
    -Verify 1 -verify_return_error > "$TEST_TMP/server.out" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 100); do
    port_free "$port" || return 0
    sleep 0.05
  done
  fail "openssl s_server did not listen"
}

serve_once() {
  serve_from "$pki/auth" "$pki/auth/ca.pem"
}

finish_server() {
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
}

talks_to_auth() {
  local client="$1" trust="$2"
  openssl s_client -connect "127.0.0.1:$port" -cert "$client/cert.pem" -key "$client/key.pem" \
    -CAfile "$trust" -verify_return_error -verify_hostname argus-auth \
    < /dev/null > "$TEST_TMP/client.out" 2>&1 && grep -q 'Verify return code: 0 (ok)' "$TEST_TMP/client.out"
}

issue_dated_cert() {
  local key="$1" subject="$2" start="$3" end="$4" out="$5"
  local dir="$TEST_TMP/dated-$(basename "$out")"

  mkdir -p "$dir/newcerts"
  : > "$dir/index.txt"
  printf '01\n' > "$dir/serial"
  cat > "$dir/ca.cnf" <<CNF
[ ca ]
default_ca = fixture_ca

[ fixture_ca ]
database = $dir/index.txt
new_certs_dir = $dir/newcerts
serial = $dir/serial
default_md = sha256
unique_subject = no
copy_extensions = none
preserve = yes
policy = fixture_policy

[ fixture_policy ]
organizationName = optional
organizationalUnitName = optional
commonName = supplied

[ fixture_ca_ext ]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
CNF
  openssl req -new -key "$key" -subj "$subject" -out "$dir/req.csr" 2> "$dir/req.log" \
    || { cat "$dir/req.log" >&2; return 1; }
  openssl ca -selfsign -batch -config "$dir/ca.cnf" -keyfile "$key" -in "$dir/req.csr" \
    -startdate "$start" -enddate "$end" -extensions fixture_ca_ext -notext -out "$out" \
    2> "$dir/ca.log" || { cat "$dir/ca.log" >&2; return 1; }
}

internal_pki_issue_ca "$pki" 30
rm -f "$pki/ca-previous.pem"
current="$(fingerprint "$pki/ca.pem")"
ensure_internal_pki "$pki" > "$TEST_TMP/rotate-first.out" 2>&1 || fail "the first pass of the CA rotation failed"
[ "$(fingerprint "$pki/ca.pem")" = "$current" ] || fail "the first pass of the CA rotation replaced the current CA"
[ -s "$pki/ca-next.pem" ] && [ "$(mode_of "$pki/ca-next.key")" = 600 ] \
  || fail "a CA with 30 days left did not get a 0600 next CA"
[ "$(days_valid "$pki/ca-next.pem")" = 3650 ] || fail "the next CA is not valid for 10 years"
[ "$(grep -c 'BEGIN CERTIFICATE' "$pki/trust.pem")" = 2 ] || fail "the first pass did not put the next CA beside the current one"
grep -q 'restart each service once, one at a time' "$TEST_TMP/rotate-first.out" \
  || fail "the first pass did not ask for a restart of each service, one at a time"
for service in $INTERNAL_PKI_SERVICES; do
  openssl verify -CAfile "$pki/ca.pem" "$pki/$service/cert.pem" >/dev/null 2>&1 \
    || fail "during the first pass the certificate of $service does not come from the current CA"
  cmp -s "$pki/$service/ca.pem" "$pki/trust.pem" || fail "$service does not carry the two-CA bundle"
done
cp -R "$pki/auth" "$TEST_TMP/old-auth"
cp -R "$pki/camera" "$TEST_TMP/old-camera"

touch -d '2 days ago' "$pki/ca-next.pem"
waiting_state="$(state_of)"
ensure_internal_pki "$pki" > "$TEST_TMP/rotate-wait.out" 2>&1 || fail "a pass before the next CA was a day old failed"
[ "$(state_of)" = "$waiting_state" ] || fail "a next CA with a fresh notBefore took over because its file looked two days old"
grep -q 'a day or more after it joined the trust bundles' "$TEST_TMP/rotate-wait.out" \
  || fail "a pass before the next CA was a day old did not say when it takes over"

issue_dated_cert "$pki/ca-next.key" "/O=Argus/OU=internal-v1/CN=$(openssl x509 -in "$pki/ca-next.pem" -noout -subject -nameopt RFC2253 | sed 's/^subject=CN=//; s/,OU=.*$//')" \
  "$(date -u -d '2 days ago' +%Y%m%d%H%M%SZ)" "$(date -u -d '3648 days' +%Y%m%d%H%M%SZ)" \
  "$pki/ca-next.pem.aged" || fail "the aged next CA could not be issued"
mv "$pki/ca-next.pem.aged" "$pki/ca-next.pem"
[ -z "$(find "$pki/ca-next.pem" -mmin +5)" ] || fail "the aged next CA does not have a fresh file time"
next="$(fingerprint "$pki/ca-next.pem")"
ensure_internal_pki "$pki" > "$TEST_TMP/rotate-second.out" 2>&1 || fail "the second pass of the CA rotation failed"
[ "$(fingerprint "$pki/ca.pem")" = "$next" ] || fail "the next CA did not take over on the second pass"
[ "$(fingerprint "$pki/ca-previous.pem")" = "$current" ] || fail "the second pass did not keep the old CA as the previous one"
[ ! -e "$pki/ca-next.pem" ] && [ ! -e "$pki/ca-next.key" ] || fail "the next CA files outlived the takeover"
[ "$(grep -c 'BEGIN CERTIFICATE' "$pki/trust.pem")" = 2 ] || fail "the trust bundle does not carry both CAs after the takeover"
grep -q 'restart each service once, one at a time' "$TEST_TMP/rotate-second.out" \
  || fail "the takeover did not ask for a restart of each service, one at a time"
for service in $INTERNAL_PKI_SERVICES; do
  openssl verify -CAfile "$pki/ca.pem" "$pki/$service/cert.pem" >/dev/null 2>&1 \
    || fail "the certificate of $service was not re-issued by the new CA"
  cmp -s "$pki/$service/ca.pem" "$pki/trust.pem" || fail "$service does not carry the rotated trust bundle"
done

serve_from "$TEST_TMP/old-auth" "$TEST_TMP/old-auth/ca.pem"
talks_to_auth "$pki/camera" "$pki/camera/ca.pem" \
  || fail "a new-CA client was refused by an old-CA server: $(tail -n 3 "$TEST_TMP/client.out")"
finish_server
serve_from "$pki/auth" "$pki/auth/ca.pem"
talks_to_auth "$TEST_TMP/old-camera" "$TEST_TMP/old-camera/ca.pem" \
  || fail "an old-CA client was refused by a new-CA server: $(tail -n 3 "$TEST_TMP/client.out")"
finish_server

internal_pki_issue_ca "$pki" 1
rm -f "$pki/ca-previous.pem"
short="$(fingerprint "$pki/ca.pem")"
ensure_internal_pki "$pki" > "$TEST_TMP/rotate-late.out" 2>&1 || fail "the pass with a CA expiring within a day failed"
[ "$(fingerprint "$pki/ca.pem")" != "$short" ] || fail "a CA expiring within a day was kept"
[ "$(fingerprint "$pki/ca-previous.pem")" = "$short" ] || fail "a CA expiring within a day was not kept as the previous one"
[ "$(days_valid "$pki/ca.pem")" = 3650 ] || fail "the CA that replaced one expiring within a day is not valid for 10 years"
grep -q 'expires within a day' "$TEST_TMP/rotate-late.out" || fail "a takeover at once did not say why"
for service in $INTERNAL_PKI_SERVICES; do
  openssl verify -CAfile "$pki/ca.pem" "$pki/$service/cert.pem" >/dev/null 2>&1 \
    || fail "after a takeover at once the certificate of $service is not from the new CA"
done

internal_pki_issue_ca "$pki" 1
rm -f "$pki/ca-previous.pem"
internal_pki_make_ca "$pki" ca-next
young="$(fingerprint "$pki/ca-next.pem")"
ensure_internal_pki "$pki" > "$TEST_TMP/rotate-late-next.out" 2>&1 \
  || fail "the pass with a CA expiring within a day and a young next CA failed"
[ "$(fingerprint "$pki/ca.pem")" = "$young" ] \
  || fail "a young next CA did not take over from a CA expiring within a day"
grep -q 'expires within a day' "$TEST_TMP/rotate-late-next.out" || fail "the early takeover did not say why"

issue_dated_cert "$pki/ca.key" "/CN=expired" 20200101000000Z 20200102000000Z "$pki/ca-previous.pem" \
  || fail "the expired previous CA could not be issued"
ensure_internal_pki "$pki" > /dev/null 2>&1 || fail "the run after the previous CA expired failed"
[ ! -e "$pki/ca-previous.pem" ] || fail "an expired previous CA was kept"
cmp -s "$pki/ca.pem" "$pki/trust.pem" || fail "an expired previous CA stayed in the trust bundle"

serve_once
openssl s_client -connect "127.0.0.1:$port" -cert "$pki/camera/cert.pem" -key "$pki/camera/key.pem" \
  -CAfile "$pki/camera/ca.pem" -verify_return_error -verify_hostname argus-auth \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "mutual TLS between camera and auth failed: $(tail -n 3 "$TEST_TMP/client.out")"
finish_server
grep -q 'Verify return code: 0 (ok)' "$TEST_TMP/client.out" || fail "camera did not verify auth's certificate"

serve_once
! openssl s_client -connect "127.0.0.1:$port" -cert "$pki/camera/cert.pem" -key "$pki/camera/key.pem" \
  -CAfile "$pki/camera/ca.pem" -verify_return_error -verify_hostname argus-identity \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "a client expecting identity accepted auth's certificate"
finish_server
grep -q 'hostname mismatch' "$TEST_TMP/client.out" || fail "the client refused auth's certificate for another reason than its name"

(umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$TEST_TMP/foreign.key")
openssl req -x509 -new -key "$TEST_TMP/foreign.key" -subj "/CN=argus-camera" -days 30 -out "$TEST_TMP/foreign.pem"
serve_once
! openssl s_client -tls1_2 -connect "127.0.0.1:$port" -cert "$TEST_TMP/foreign.pem" -key "$TEST_TMP/foreign.key" \
  -CAfile "$pki/camera/ca.pem" -verify_return_error \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "auth accepted a client certificate the internal CA did not issue"
finish_server
grep -q 'certificate verify failed' "$TEST_TMP/server.out" || fail "auth refused the foreign client for another reason than its issuer"

serve_once
! openssl s_client -tls1_2 -connect "127.0.0.1:$port" -CAfile "$pki/camera/ca.pem" -verify_return_error \
  < /dev/null > "$TEST_TMP/client.out" 2>&1 || fail "auth accepted a client without a certificate"
finish_server
grep -q 'peer did not return a certificate' "$TEST_TMP/server.out" \
  || fail "auth refused the client without a certificate for another reason"

traced="$TEST_TMP/traced"
{
  printf 'set -euo pipefail\nsource %q\nsource %q\n' "$ROOT/scripts/lib/common.sh" "$ROOT/scripts/lib/internal-pki.sh"
  printf 'ensure_internal_pki %q\n' "$traced"
  printf 'cp -R %q %q\n' "$traced" "$TEST_TMP/first-keys"
  printf 'internal_pki_issue_ca %q 30\n' "$traced"
  printf 'ensure_internal_pki %q\n' "$traced"
} > "$TEST_TMP/traced.sh"
bash -x "$TEST_TMP/traced.sh" > /dev/null 2> "$TEST_TMP/trace" || fail "the traced issuance failed"
find "$traced" "$TEST_TMP/first-keys" -type f \( -name '*.key' -o -name 'key.pem' \) -exec cat {} + \
  | awk '
      /^-----/ { next }
      {
        print
        count = split($0, part, /[^A-Za-z0-9]+/)
        for (i = 1; i <= count; i++)
          if (length(part[i]) >= 16 && part[i] != $0)
            print part[i]
      }' | sort -u > "$TEST_TMP/key-values"
[ "$(wc -l < "$TEST_TMP/key-values")" -ge 84 ] || fail "the trace check gathered too few key lines"
! grep -qF -f "$TEST_TMP/key-values" "$TEST_TMP/trace" || fail "the set -x trace of the issuance carries a private key"

native="$TEST_TMP/native"
mkdir -p "$native/services/tts" "$native/services/auth" "$native/services/stt"
printf '[server]\nport = 7029\n\n[internal_tls]\ndir = ""\naddress = "127.0.0.1:7329"\n' \
  > "$native/services/tts/config.toml"
printf '[server]\nport = 7042\n' > "$native/services/auth/config.toml"
printf '[internal_tls]\ndir = ""\n' > "$native/services/stt/config.toml"
mv "$pki/stt/cert.pem" "$TEST_TMP/stt-cert.pem"
(cd "$TEST_TMP" && internal_pki_configure_native "$native" certs/internal)
mv "$TEST_TMP/stt-cert.pem" "$pki/stt/cert.pem"
absolute="$(cd "$pki" && pwd -P)"
[ "$(toml_value "$native/services/tts/config.toml" internal_tls dir)" = "$absolute/tts" ] \
  || fail "setup.sh's native pass did not point tts at its own certificate directory by an absolute path"
[ "$(toml_value "$native/services/tts/config.toml" internal_tls address)" = "127.0.0.1:7329" ] \
  || fail "the native pass changed the listener address"
! toml_key_exists "$native/services/auth/config.toml" internal_tls dir \
  || fail "the native pass gave a service whose config has no [internal_tls] one"
[ -z "$(toml_value "$native/services/stt/config.toml" internal_tls dir)" ] \
  || fail "the native pass pointed a service at a directory without its certificate"
native_state="$(sha256sum "$native/services/tts/config.toml")"
internal_pki_configure_native "$native" "$pki"
[ "$(sha256sum "$native/services/tts/config.toml")" = "$native_state" ] || fail "a second native pass changed the config"
[ "$(mode_of "$native/services/tts/config.toml")" = 600 ] || fail "the native pass left the config readable by others"
internal_pki_configure_native "$native" "$TEST_TMP/absent" || fail "the native pass failed without a PKI directory"

grep -Eq '^  ensure_internal_pki "\$DATA_DIR/certs/internal"$' "$ROOT/scripts/provision-host.sh" \
  || fail "provision-host.sh does not issue and renew the internal certificates"
grep -Eq '^  ensure_internal_pki "\$\{ARGUS_DATA_DIR:-\$ROOT/argus-deploy/data\}/certs/internal"$' "$ROOT/scripts/setup.sh" \
  || fail "setup.sh does not issue and renew the internal certificates natively"
grep -Eq '^  internal_pki_configure_native "\$ROOT" "\$\{ARGUS_DATA_DIR:-\$ROOT/argus-deploy/data\}/certs/internal"$' \
  "$ROOT/scripts/setup.sh" || fail "setup.sh does not point the native configs at their internal certificates"

printf 'internal-pki-test: ok\n'
