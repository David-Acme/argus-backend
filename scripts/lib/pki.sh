#!/usr/bin/env bash

pki_private_address() {
  case "$1" in
    10.*|192.168.*|169.254.*) return 0 ;;
    172.1[6-9].*|172.2[0-9].*|172.3[01].*) return 0 ;;
    100.6[4-9].*|100.[7-9][0-9].*|100.1[01][0-9].*|100.12[0-7].*) return 0 ;;
  esac
  return 1
}

pki_dns_label_ok() {
  case "$1" in
    ""|*[!A-Za-z0-9.-]*|.*|*.) return 1 ;;
  esac
  return 0
}

pki_name_constraints() {
  local host="$1"
  local mdns_name="$2"
  local remote_host="$3"
  local permitted="permitted;DNS:local,permitted;DNS:localhost"
  local name range

  for name in "$host" "$mdns_name" "$remote_host" ${ARGUS_CA_EXTRA_DNS:-}; do
    pki_dns_label_ok "$name" || continue
    case "$name" in *.local|local|localhost) continue ;; esac
    permitted="$permitted,permitted;DNS:$name"
  done
  for range in 127.0.0.0/255.0.0.0 10.0.0.0/255.0.0.0 172.16.0.0/255.240.0.0 \
      192.168.0.0/255.255.0.0 169.254.0.0/255.255.0.0 100.64.0.0/255.192.0.0 \
      ::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff fc00::/fe00:: fe80::/ffc0::; do
    permitted="$permitted,permitted;IP:$range"
  done
  printf 'critical,%s' "$permitted"
}

ensure_instance_certs() {
  local root="$1"
  local cert_dir="${2:-$1/certs}"
  local mdns_source="${3:-}"
  local signer_dir="${4:-$cert_dir}"
  local lan_address="${5:-}"

  log "Setting up local PKI (instance CA + server certificate)..."
  need_cmd openssl
  need_cmd base32
  mkdir -p "$cert_dir" "$signer_dir"
  if [ "$signer_dir" != "$cert_dir" ]; then
    chmod 700 "$signer_dir"
    if [ -f "$cert_dir/ca.key" ] && [ ! -f "$signer_dir/ca.key" ]; then
      mv "$cert_dir/ca.key" "$signer_dir/ca.key"
      log "Moved the CA key out of the shared certs dir into $signer_dir"
    fi
    if [ -f "$cert_dir/pairing.code" ] && [ ! -f "$signer_dir/pairing.code" ]; then
      mv "$cert_dir/pairing.code" "$signer_dir/pairing.code"
    fi
    rm -f "$cert_dir/ca.srl"
  fi

  if [ ! -f "$cert_dir/ca.pem" ]; then
    log "Generating instance CA (10 years, name-constrained) and server leaf (90 days)..."

    local mdns_name=""
    local remote_host=""
    if [ -n "$mdns_source" ] && [ -f "$mdns_source" ]; then
      mdns_name="$(sed -n 's/^[[:space:]]*name *= *"\([^"]*\)".*/\1/p' "$mdns_source" | head -1)"
      remote_host="$(sed -n 's/^[[:space:]]*hostname *= *"\([^"]*\)".*/\1/p' "$mdns_source" | head -1)"
    fi
    [ -z "$mdns_name" ] && mdns_name="Argus"
    local host
    host="$(hostname 2>/dev/null || true)"

    openssl ecparam -name prime256v1 -genkey -noout -out "$signer_dir/ca.key"
    chmod 600 "$signer_dir/ca.key"
    openssl req -x509 -new -key "$signer_dir/ca.key" -sha256 -days 3650 \
      -subj "/CN=Argus Instance CA" \
      -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
      -addext "keyUsage=critical,keyCertSign,cRLSign" \
      -addext "nameConstraints=$(pki_name_constraints "$host" "$mdns_name" "$remote_host")" \
      -out "$cert_dir/ca.pem"

    openssl ecparam -name prime256v1 -genkey -noout -out "$cert_dir/server.key"

    local san="DNS:argus.local,DNS:localhost,IP:127.0.0.1,IP:::1"
    pki_dns_label_ok "$host" && san="$san,DNS:$host"
    case "x$mdns_name" in
      x[A-Za-z0-9_-]*) san="$san,DNS:${mdns_name}" ;;
    esac
    if pki_private_address "$lan_address"; then
      san="$san,IP:$lan_address"
    fi

    openssl req -new -key "$cert_dir/server.key" \
      -subj "/CN=${mdns_name}.local" -out "$cert_dir/server.csr"
    openssl x509 -req -in "$cert_dir/server.csr" \
      -CA "$cert_dir/ca.pem" -CAkey "$signer_dir/ca.key" \
      -CAserial "$signer_dir/ca.srl" -CAcreateserial \
      -sha256 -days 90 \
      -extfile <(printf 'subjectAltName=%s\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' "$san") \
      -out "$cert_dir/server.pem"
    rm -f "$cert_dir/server.csr"

    cat "$cert_dir/ca.pem" >> "$cert_dir/server.pem"
    chmod 600 "$cert_dir/server.key"
  else
    log "PKI already present."
  fi

  local fingerprint
  fingerprint="$(openssl x509 -in "$cert_dir/ca.pem" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g')"
  local code_file="$signer_dir/pairing.code"
  local code=""
  [ -f "$code_file" ] && code="$(tr -d '[:space:]' < "$code_file")"
  if ! printf '%s' "$code" | grep -Eq '^[A-Z2-7]{26,32}$'; then
    code="$(openssl rand 16 | base32 | tr -d '=\n')"
    (umask 077 && printf '%s\n' "$code" > "$code_file")
  fi
  chmod 600 "$code_file"

  log "CA fingerprint (SHA-256): $fingerprint"
  log "Pairing code: $code"
}
