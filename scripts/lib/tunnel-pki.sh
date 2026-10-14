#!/usr/bin/env bash

TUNNEL_PKI_SERVICES="auth identity sync camera productivity notification guard settings rtc turn"
TUNNEL_PKI_DOMAIN="argus-tunnel.lynk.page"
TUNNEL_PKI_LEAF_DAYS=730
TUNNEL_PKI_RENEW_DAYS=60

tunnel_pki_name() {
  printf '%s--%s.%s\n' "$1" "$2" "$TUNNEL_PKI_DOMAIN"
}

tunnel_pki_subject() {
  printf '/O=Argus/OU=tunnel/CN=Argus tunnel %s\n' "$1"
}

tunnel_pki_home_id() {
  local config="$1"
  local home_id

  [ -f "$config" ] || return 1
  home_id="$(toml_value "$config" link home_id)"
  case "$home_id" in
    ""|*[!0-9a-f]*) return 1 ;;
  esac
  [ "${#home_id}" -eq 32 ] || return 1
  printf '%s\n' "$home_id"
}

tunnel_pki_ca_names() {
  local home_id service names=""

  home_id="$(tunnel_pki_home_id "$1")" || return 0
  for service in $TUNNEL_PKI_SERVICES; do
    names="$names $(tunnel_pki_name "$service" "$home_id")"
  done
  printf '%s\n' "${names# }"
}

tunnel_pki_leaf_extensions() {
  printf 'subjectAltName=DNS:%s\n' "$1"
  printf 'basicConstraints=critical,CA:FALSE\n'
  printf 'keyUsage=critical,digitalSignature\n'
  printf 'extendedKeyUsage=serverAuth\n'
  printf 'subjectKeyIdentifier=hash\n'
  printf 'authorityKeyIdentifier=keyid\n'
}

tunnel_pki_san() {
  openssl x509 -in "$1" -noout -ext subjectAltName 2>/dev/null | tail -n +2 | tr -d ' \n'
}

tunnel_pki_leaf_current() {
  local leaf="$1"
  local name="$2"
  local ca_pem="$3"
  local service

  [ -s "$leaf/key.pem" ] && [ -s "$leaf/cert.pem" ] || return 1
  openssl verify -CAfile "$ca_pem" -purpose sslserver "$leaf/cert.pem" >/dev/null 2>&1 || return 1
  internal_pki_valid_beyond "$leaf/cert.pem" "$TUNNEL_PKI_RENEW_DAYS" || return 1
  internal_pki_pair_matches "$leaf/cert.pem" "$leaf/key.pem" || return 1
  service="${name%%--*}"
  [ "$(internal_pki_subject "$leaf/cert.pem")" = "CN=Argus tunnel $service,OU=tunnel,O=Argus" ] || return 1
  [ "$(tunnel_pki_san "$leaf/cert.pem")" = "DNS:$name" ]
}

tunnel_pki_issue_leaf() {
  local leaf="$1"
  local name="$2"
  local ca_pem="$3"
  local ca_key="$4"
  local days="${5:-$TUNNEL_PKI_LEAF_DAYS}"

  mkdir -p "$leaf"
  chmod 700 "$leaf"
  rm -f "$leaf/key.pem.new" "$leaf/cert.pem.new" "$leaf/cert.csr" "$leaf/issue.log"
  (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$leaf/key.pem.new")
  openssl req -new -key "$leaf/key.pem.new" -subj "$(tunnel_pki_subject "${name%%--*}")" \
    -out "$leaf/cert.csr"
  if ! openssl x509 -req -in "$leaf/cert.csr" -CA "$ca_pem" -CAkey "$ca_key" \
      -set_serial "0x$(openssl rand -hex 16)" -sha256 -days "$days" \
      -extfile <(tunnel_pki_leaf_extensions "$name") \
      -out "$leaf/cert.pem.new" 2> "$leaf/issue.log"; then
    cat "$leaf/issue.log" >&2
    rm -f "$leaf/key.pem.new" "$leaf/cert.pem.new" "$leaf/cert.csr" "$leaf/issue.log"
    err "Tunnel PKI: the certificate for $name could not be issued"
    return 1
  fi
  rm -f "$leaf/cert.csr" "$leaf/issue.log"
  if ! openssl verify -CAfile "$ca_pem" -purpose sslserver "$leaf/cert.pem.new" >/dev/null 2>&1; then
    rm -f "$leaf/key.pem.new" "$leaf/cert.pem.new"
    return 2
  fi
  cat "$ca_pem" >> "$leaf/cert.pem.new"
  chmod 600 "$leaf/key.pem.new"
  chmod 644 "$leaf/cert.pem.new"
  mv "$leaf/key.pem.new" "$leaf/key.pem"
  mv "$leaf/cert.pem.new" "$leaf/cert.pem"
}

ensure_tunnel_pki() {
  local dir="$1"
  local ca_pem="$2"
  local ca_key="$3"
  local tunnel_config="$4"
  local home_id service name code issued="" refused=""

  if ! home_id="$(tunnel_pki_home_id "$tunnel_config")"; then
    log "Tunnel PKI: $tunnel_config names no home id yet, so no device-name certificate is issued"
    return 0
  fi
  if [ ! -s "$ca_pem" ] || [ ! -s "$ca_key" ]; then
    warn "Tunnel PKI: the instance CA is not at $ca_pem and $ca_key, so no device-name certificate is issued"
    return 0
  fi
  need_cmd openssl
  mkdir -p "$dir"
  chmod 700 "$dir"
  for service in $TUNNEL_PKI_SERVICES; do
    name="$(tunnel_pki_name "$service" "$home_id")"
    if tunnel_pki_leaf_current "$dir/$service" "$name" "$ca_pem"; then
      chmod 600 "$dir/$service/key.pem"
      continue
    fi
    code=0
    tunnel_pki_issue_leaf "$dir/$service" "$name" "$ca_pem" "$ca_key" || code=$?
    case "$code" in
      0) issued="$issued $service" ;;
      2) refused="$refused $service" ;;
      *) return "$code" ;;
    esac
  done
  if [ -n "$issued" ]; then
    log "Tunnel PKI: issued ${TUNNEL_PKI_LEAF_DAYS}-day device-name certificates for${issued} (home $home_id)"
  fi
  if [ -n "$refused" ]; then
    warn "Tunnel PKI: the instance CA does not permit the device names of${refused}; its name constraints were fixed when it was made"
    warn "Tunnel PKI: those services keep presenting the instance certificate on their tunnel listener; a CA that permits the names means a new CA and every device paired again"
  fi
  if [ -z "$issued" ] && [ -z "$refused" ]; then
    log "Tunnel PKI: every device-name certificate is valid for more than $TUNNEL_PKI_RENEW_DAYS days"
  fi
}

tunnel_pki_configure_native() {
  local root="$1"
  local dir="$2"
  local service config

  for service in $TUNNEL_PKI_SERVICES; do
    config="$root/services/$service/config.toml"
    [ -f "$config" ] && [ -s "$dir/$service/cert.pem" ] || continue
    replace_toml_value remote tunnel_cert "$dir/$service/cert.pem" "$config"
    replace_toml_value remote tunnel_key "$dir/$service/key.pem" "$config"
  done
}
