#!/usr/bin/env bash

INTERNAL_PKI_SERVICES="auth camera guard identity llm notification productivity settings stt sync tts vlm voice"
INTERNAL_PKI_CA_DAYS=3650
INTERNAL_PKI_LEAF_DAYS=730
INTERNAL_PKI_RENEW_DAYS=60
INTERNAL_PKI_PROMOTE_AFTER_DAYS=1
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

internal_pki_ca_usable() {
  local cert="$1"
  local key="$2"

  [ -s "$key" ] && [ -s "$cert" ] || return 1
  internal_pki_valid_beyond "$cert" 0 || return 1
  internal_pki_pair_matches "$cert" "$key" || return 1
  case "$(internal_pki_subject "$cert")" in
    "CN=Argus internal CA "*",OU=$INTERNAL_PKI_PROFILE,O=Argus") return 0 ;;
  esac
  return 1
}

internal_pki_make_ca() {
  local dir="$1"
  local name="$2"
  local days="${3:-$INTERNAL_PKI_CA_DAYS}"

  rm -f "$dir/$name.key.new" "$dir/$name.pem.new"
  (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$dir/$name.key.new")
  openssl req -x509 -new -key "$dir/$name.key.new" -sha256 -days "$days" \
    -subj "/O=Argus/OU=$INTERNAL_PKI_PROFILE/CN=Argus internal CA $(openssl rand -hex 4)" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$dir/$name.pem.new"
  chmod 600 "$dir/$name.key.new"
  chmod 644 "$dir/$name.pem.new"
  mv "$dir/$name.key.new" "$dir/$name.key"
  mv "$dir/$name.pem.new" "$dir/$name.pem"
}

internal_pki_promote() {
  local dir="$1"

  if [ -s "$dir/ca.pem" ]; then
    mv "$dir/ca.pem" "$dir/ca-previous.pem"
  fi
  mv "$dir/ca-next.key" "$dir/ca.key"
  mv "$dir/ca-next.pem" "$dir/ca.pem"
}

internal_pki_issue_ca() {
  local dir="$1"
  local days="${2:-$INTERNAL_PKI_CA_DAYS}"

  rm -f "$dir/ca-next.key" "$dir/ca-next.pem"
  internal_pki_make_ca "$dir" ca-next "$days"
  internal_pki_promote "$dir"
}

internal_pki_issued_seconds_ago() {
  local start
  start="$(openssl x509 -in "$1" -noout -startdate 2>/dev/null | sed 's/^notBefore=//')"
  [ -n "$start" ] || return 1
  start="$(date -u -d "$start" +%s 2>/dev/null)" || return 1
  printf '%s\n' "$(( $(date -u +%s) - start ))"
}

internal_pki_next_ripe() {
  local age
  age="$(internal_pki_issued_seconds_ago "$1/ca-next.pem")" || return 1
  [ "$age" -ge "$(( INTERNAL_PKI_PROMOTE_AFTER_DAYS * 86400 ))" ]
}

internal_pki_rotate() {
  local dir="$1"

  if [ -e "$dir/ca-next.pem" ] || [ -e "$dir/ca-next.key" ]; then
    internal_pki_ca_usable "$dir/ca-next.pem" "$dir/ca-next.key" \
      || rm -f "$dir/ca-next.pem" "$dir/ca-next.key"
  fi
  if ! internal_pki_ca_usable "$dir/ca.pem" "$dir/ca.key"; then
    if [ -s "$dir/ca-next.pem" ]; then
      internal_pki_promote "$dir"
      warn "Internal PKI: the internal CA was unusable, so the next one took over and every internal certificate is re-issued; restart every service"
    elif [ -e "$dir/ca.pem" ]; then
      internal_pki_issue_ca "$dir"
      warn "Internal PKI: the internal CA was unusable and was issued again; every internal certificate is re-issued, so restart every service"
    else
      internal_pki_issue_ca "$dir"
      log "Internal PKI: internal CA issued in $dir ($INTERNAL_PKI_CA_DAYS days)"
    fi
    return 0
  fi
  if [ ! -s "$dir/ca-next.pem" ]; then
    internal_pki_valid_beyond "$dir/ca.pem" "$INTERNAL_PKI_RENEW_DAYS" && return 0
    internal_pki_make_ca "$dir" ca-next
    if internal_pki_valid_beyond "$dir/ca.pem" "$INTERNAL_PKI_PROMOTE_AFTER_DAYS"; then
      warn "Internal PKI: the internal CA has fewer than $INTERNAL_PKI_RENEW_DAYS days left, so a next CA joins every trust bundle; restart each service once, one at a time (every certificate in use still comes from the current CA), then run provisioning again a day or more from now to move the certificates to the next CA"
      return 0
    fi
  elif ! internal_pki_next_ripe "$dir" && internal_pki_valid_beyond "$dir/ca.pem" "$INTERNAL_PKI_PROMOTE_AFTER_DAYS"; then
    log "Internal PKI: the next internal CA takes over on a provisioning pass a day or more after it joined the trust bundles"
    return 0
  fi
  if internal_pki_valid_beyond "$dir/ca.pem" "$INTERNAL_PKI_PROMOTE_AFTER_DAYS"; then
    internal_pki_promote "$dir"
    warn "Internal PKI: the next internal CA took over and every internal certificate is re-issued; restart each service once, one at a time (every service trusts both CAs until the old one expires)"
  else
    internal_pki_promote "$dir"
    warn "Internal PKI: the internal CA expires within a day, so the next one took over at once and every internal certificate is re-issued; restart every service"
  fi
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
    if [ -s "$dir/ca-next.pem" ]; then
      cat "$dir/ca-next.pem"
    fi
    if [ -f "$dir/ca-previous.pem" ]; then
      cat "$dir/ca-previous.pem"
    fi
  } > "$dir/trust.pem.candidate"
  internal_pki_install "$dir/trust.pem.candidate" "$dir/trust.pem"
  rm -f "$dir/trust.pem.candidate"
}

ensure_internal_pki() {
  local dir="$1"
  local service issued=""

  need_cmd openssl
  mkdir -p "$dir"
  chmod 700 "$dir"
  internal_pki_rotate "$dir"
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
  if [ -e "$dir/ca-next.key" ]; then
    chmod 600 "$dir/ca-next.key"
  fi
  if [ -n "$issued" ]; then
    log "Internal PKI: issued ${INTERNAL_PKI_LEAF_DAYS}-day certificates for${issued}"
  else
    log "Internal PKI: every internal certificate is valid for more than $INTERNAL_PKI_RENEW_DAYS days"
  fi
}

internal_pki_native_dial_rows() {
  cat <<'ROWS'
auth identity target identity
camera identity target identity
camera modules target settings
guard camera actions_target camera
guard notifications grpc_target notification
guard identity target identity
guard modules target settings
identity auth target auth
identity sync control_target sync
identity modules target settings
llm guard target guard
llm productivity grpc_target productivity
llm modules target settings
llm notifications target notification
notification identity target identity
notification sync control_target sync
notification voice target voice
notification modules target settings
productivity identity target identity
productivity notifications target notification
productivity modules target settings
settings auth target auth
sync camera grpc_target camera
sync identity target identity
sync productivity grpc_target productivity
sync voice target voice
sync notifications grpc_target notification
sync guard presence_target guard
sync modules target settings
voice identity target identity
voice notification target notification
ROWS
  local owner
  for owner in llm voice tts stt vlm guard camera notification identity productivity sync; do
    printf 'settings owners.%s target %s\n' "$owner" "$owner"
  done
}

internal_pki_tls_port_of() {
  local root="$1"
  local service="$2"
  local config port

  for config in "$root/services/$service/config.toml" "$root/services/$service/config.toml.example"; do
    [ -f "$config" ] || continue
    port="$(settings_owner_tls_port "$config")"
    if [ -n "$port" ]; then
      printf '%s' "$port"
      return 0
    fi
  done
  return 1
}

migrate_native_dial_targets() {
  local root="$1"
  local dialer table key callee config dir value tls_port
  local -A unpaired=()

  while read -r dialer table key callee; do
    config="$root/services/$dialer/config.toml"
    [ -f "$config" ] && toml_key_exists "$config" "$table" "$key" || continue
    value="$(toml_value "$config" "$table" "$key")"
    case "$value" in
      127.0.0.1:[0-9]*) ;;
      *) continue ;;
    esac
    dir="$(toml_value "$config" internal_tls dir)"
    if [ -z "$dir" ] || [ ! -s "$dir/cert.pem" ]; then
      if [ -z "${unpaired[$dialer]:-}" ]; then
        unpaired[$dialer]=1
        warn "services/$dialer/config.toml has no internal certificate, so its dial targets stay plaintext; run scripts/setup.sh to issue the internal PKI"
      fi
      continue
    fi
    tls_port="$(internal_pki_tls_port_of "$root" "$callee")" || {
      warn "services/$callee has no [internal_tls] address; services/$dialer/config.toml [$table] $key stays plaintext"
      continue
    }
    replace_toml_value "$table" "$key" "tls:127.0.0.1:$tls_port" "$config"
    log "services/$dialer/config.toml [$table] $key: $value -> tls:127.0.0.1:$tls_port"
  done < <(internal_pki_native_dial_rows)
}

internal_pki_configure_native() {
  local root="$1"
  local dir="$2"
  local service config

  [ -d "$dir" ] || return 0
  dir="$(cd "$dir" && pwd -P)"
  for service in $INTERNAL_PKI_SERVICES; do
    config="$root/services/$service/config.toml"
    [ -f "$config" ] && [ -s "$dir/$service/cert.pem" ] || continue
    toml_key_exists "$config" internal_tls dir || continue
    replace_toml_value internal_tls dir "$dir/$service" "$config"
  done
}
