#!/usr/bin/env bash

INTERNAL_PKI_SERVICES="auth camera guard identity llm notification productivity settings stt sync tts vlm voice"
INTERNAL_PKI_CA_DAYS=3650
INTERNAL_PKI_LEAF_DAYS=730
INTERNAL_PKI_RENEW_DAYS=60
INTERNAL_PKI_PROFILE="internal-v1"

internal_pki_valid_beyond() {
  openssl x509 -in "$1" -noout -checkend "$(( $2 * 86400 ))" >/dev/null 2>&1
}

internal_pki_pair_matches() {
  local scratch matched=1
  scratch="$(mktemp -d)"
  if openssl x509 -in "$1" -noout -pubkey > "$scratch/cert" 2>/dev/null &&
    openssl pkey -in "$2" -pubout > "$scratch/key" 2>/dev/null &&
    [ -s "$scratch/cert" ] && cmp -s "$scratch/cert" "$scratch/key"; then
    matched=0
  fi
  rm -rf "$scratch"
  return "$matched"
}

internal_pki_subject() {
  openssl x509 -in "$1" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject=[[:space:]]*//'
}

internal_pki_ca_current() {
  local dir="$1"
  [ -s "$dir/ca.key" ] && [ -s "$dir/ca.pem" ] || return 1
  internal_pki_valid_beyond "$dir/ca.pem" "$INTERNAL_PKI_RENEW_DAYS" || return 1
  internal_pki_pair_matches "$dir/ca.pem" "$dir/ca.key" || return 1
  case "$(internal_pki_subject "$dir/ca.pem")" in
    "CN=Argus internal CA "*",OU=$INTERNAL_PKI_PROFILE,O=Argus") return 0 ;;
  esac
  return 1
}

internal_pki_issue_ca() {
  local dir="$1"
  local days="${2:-$INTERNAL_PKI_CA_DAYS}"

  rm -f "$dir/ca.key.new" "$dir/ca.pem.new"
  (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$dir/ca.key.new")
  openssl req -x509 -new -key "$dir/ca.key.new" -sha256 -days "$days" \
    -subj "/O=Argus/OU=$INTERNAL_PKI_PROFILE/CN=Argus internal CA $(openssl rand -hex 4)" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$dir/ca.pem.new"
  chmod 600 "$dir/ca.key.new"
  chmod 644 "$dir/ca.pem.new"
  if [ -s "$dir/ca.pem" ]; then
    mv "$dir/ca.pem" "$dir/ca-previous.pem"
  fi
  mv "$dir/ca.key.new" "$dir/ca.key"
  mv "$dir/ca.pem.new" "$dir/ca.pem"
}

internal_pki_leaf_extensions() {
  printf 'subjectAltName=DNS:argus-%s,DNS:localhost,IP:127.0.0.1,IP:::1\n' "$1"
  printf 'basicConstraints=critical,CA:FALSE\n'
  printf 'keyUsage=critical,digitalSignature\n'
  printf 'extendedKeyUsage=serverAuth,clientAuth\n'
  printf 'subjectKeyIdentifier=hash\n'
  printf 'authorityKeyIdentifier=keyid\n'
}

internal_pki_leaf_current() {
  local dir="$1"
  local service="$2"
  local leaf="$dir/$service"

  [ -s "$leaf/key.pem" ] && [ -s "$leaf/cert.pem" ] || return 1
  openssl verify -CAfile "$dir/ca.pem" "$leaf/cert.pem" >/dev/null 2>&1 || return 1
  internal_pki_valid_beyond "$leaf/cert.pem" "$INTERNAL_PKI_RENEW_DAYS" || return 1
  internal_pki_pair_matches "$leaf/cert.pem" "$leaf/key.pem" || return 1
  [ "$(internal_pki_subject "$leaf/cert.pem")" = "CN=argus-$service,OU=$INTERNAL_PKI_PROFILE,O=Argus" ]
}

internal_pki_issue_leaf() {
  local dir="$1"
  local service="$2"
  local days="${3:-$INTERNAL_PKI_LEAF_DAYS}"
  local leaf="$dir/$service"

  mkdir -p "$leaf"
  chmod 700 "$leaf"
  rm -f "$leaf/key.pem.new" "$leaf/cert.pem.new" "$leaf/cert.csr" "$leaf/issue.log"
  (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$leaf/key.pem.new")
  openssl req -new -key "$leaf/key.pem.new" \
    -subj "/O=Argus/OU=$INTERNAL_PKI_PROFILE/CN=argus-$service" -out "$leaf/cert.csr"
  if ! openssl x509 -req -in "$leaf/cert.csr" -CA "$dir/ca.pem" -CAkey "$dir/ca.key" \
      -set_serial "0x$(openssl rand -hex 16)" -sha256 -days "$days" \
      -extfile <(internal_pki_leaf_extensions "$service") \
      -out "$leaf/cert.pem.new" 2> "$leaf/issue.log"; then
    cat "$leaf/issue.log" >&2
    rm -f "$leaf/key.pem.new" "$leaf/cert.pem.new" "$leaf/cert.csr" "$leaf/issue.log"
    err "Internal PKI: the certificate of $service could not be issued"
    return 1
  fi
  rm -f "$leaf/cert.csr" "$leaf/issue.log"
  chmod 600 "$leaf/key.pem.new"
  chmod 644 "$leaf/cert.pem.new"
  mv "$leaf/key.pem.new" "$leaf/key.pem"
  mv "$leaf/cert.pem.new" "$leaf/cert.pem"
}

internal_pki_install() {
  local source="$1"
  local target="$2"

  if [ -f "$target" ] && cmp -s "$source" "$target"; then
    return 0
  fi
  cp "$source" "$target.new"
  chmod 644 "$target.new"
  mv "$target.new" "$target"
}

internal_pki_trust() {
  local dir="$1"

  if [ -f "$dir/ca-previous.pem" ] && ! internal_pki_valid_beyond "$dir/ca-previous.pem" 0; then
    rm -f "$dir/ca-previous.pem"
  fi
  {
    cat "$dir/ca.pem"
    if [ -f "$dir/ca-previous.pem" ]; then
      cat "$dir/ca-previous.pem"
    fi
  } > "$dir/trust.pem.candidate"
  internal_pki_install "$dir/trust.pem.candidate" "$dir/trust.pem"
  rm -f "$dir/trust.pem.candidate"
}

ensure_internal_pki() {
  local dir="$1"
  local service had_ca=0 issued=""

  need_cmd openssl
  mkdir -p "$dir"
  chmod 700 "$dir"
  [ ! -s "$dir/ca.pem" ] || had_ca=1
  if ! internal_pki_ca_current "$dir"; then
    internal_pki_issue_ca "$dir"
    if [ "$had_ca" -eq 1 ]; then
      warn "Internal PKI: a new internal CA was issued in $dir; every internal certificate is re-issued, so restart every service once (docker compose restart)"
    else
      log "Internal PKI: internal CA issued in $dir ($INTERNAL_PKI_CA_DAYS days)"
    fi
  fi
  internal_pki_trust "$dir"
  for service in $INTERNAL_PKI_SERVICES; do
    if ! internal_pki_leaf_current "$dir" "$service"; then
      internal_pki_issue_leaf "$dir" "$service"
      issued="$issued $service"
    fi
    chmod 700 "$dir/$service"
    chmod 600 "$dir/$service/key.pem"
    internal_pki_install "$dir/trust.pem" "$dir/$service/ca.pem"
  done
  chmod 600 "$dir/ca.key"
  if [ -n "$issued" ]; then
    log "Internal PKI: issued ${INTERNAL_PKI_LEAF_DAYS}-day certificates for${issued}"
  else
    log "Internal PKI: every internal certificate is valid for more than $INTERNAL_PKI_RENEW_DAYS days"
  fi
}
