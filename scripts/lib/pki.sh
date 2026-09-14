#!/usr/bin/env bash
# Instance PKI: one self-signed EC CA plus a rotating server leaf. Callers
# source lib/common.sh first for log/warn/need_cmd.

ensure_instance_certs() {
  local root="$1"
  local cert_dir="${2:-$1/certs}"
  local mdns_source="${3:-}"

  log "Setting up local PKI (instance CA + server certificate)..."
  need_cmd openssl
  mkdir -p "$cert_dir"

  if [ ! -f "$cert_dir/ca.pem" ]; then
    log "Generating instance CA (25 years) and server leaf (90 days)..."

    openssl ecparam -name prime256v1 -genkey -noout -out "$cert_dir/ca.key"
    openssl req -x509 -new -key "$cert_dir/ca.key" -sha256 -days 9125 \
      -subj "/CN=Argus Instance CA" -out "$cert_dir/ca.pem"

    openssl ecparam -name prime256v1 -genkey -noout -out "$cert_dir/server.key"

    local mdns_name=""
    if [ -n "$mdns_source" ] && [ -f "$mdns_source" ]; then
      mdns_name="$(sed -n 's/^[[:space:]]*name *= *"\([^"]*\)".*/\1/p' "$mdns_source" | head -1)"
    fi
    [ -z "$mdns_name" ] && mdns_name="Argus"
    local san="DNS:argus.local,DNS:localhost,IP:127.0.0.1,IP:::1"
    local host="$(hostname 2>/dev/null)"
    [ -n "$host" ] && san="$san,DNS:$host"
    case "x$mdns_name" in
      x[A-Za-z0-9_-]*) san="$san,DNS:${mdns_name}" ;;
    esac

    openssl req -new -key "$cert_dir/server.key" \
      -subj "/CN=${mdns_name}.local" -out "$cert_dir/server.csr"
    openssl x509 -req -in "$cert_dir/server.csr" \
      -CA "$cert_dir/ca.pem" -CAkey "$cert_dir/ca.key" -CAcreateserial \
      -sha256 -days 90 -extfile <(printf "subjectAltName=%s" "$san") \
      -out "$cert_dir/server.pem"
    rm -f "$cert_dir/server.csr"

    cat "$cert_dir/ca.pem" >> "$cert_dir/server.pem"
    chmod 600 "$cert_dir/ca.key" "$cert_dir/server.key"
  else
    log "PKI already present."
  fi

  local fingerprint
  fingerprint="$(openssl x509 -in "$cert_dir/ca.pem" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g')"
  echo "${fingerprint:0:8}" > "$cert_dir/pairing.code"
  chmod 600 "$cert_dir/pairing.code"

  log "CA fingerprint (SHA-256): $fingerprint"
  log "Pairing code: ${fingerprint:0:8}"
}
