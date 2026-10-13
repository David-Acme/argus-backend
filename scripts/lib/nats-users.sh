#!/usr/bin/env bash

NKEY_ED25519_PKCS8_PREFIX="302e020100300506032b657004220420"
NKEY_USER_SEED_PREFIX="9500"
NKEY_USER_PREFIX="a0"

nkey_crc16() {
  local hex="$1" crc=0 at
  for (( at = 0; at < ${#hex}; at += 2 )); do
    crc=$(( crc ^ (16#${hex:at:2} << 8) ))
    for _ in 1 2 3 4 5 6 7 8; do
      if (( crc & 0x8000 )); then
        crc=$(( ((crc << 1) ^ 0x1021) & 0xFFFF ))
      else
        crc=$(( (crc << 1) & 0xFFFF ))
      fi
    done
  done
  printf '%02x%02x' $(( crc & 0xFF )) $(( crc >> 8 ))
}

nkey_binary() {
  local hex="$1" at escaped=""
  for (( at = 0; at < ${#hex}; at += 2 )); do
    escaped+="\\x${hex:at:2}"
  done
  printf '%b' "$escaped"
}

nkey_encode() {
  local raw="$1$2"
  nkey_binary "$raw$(nkey_crc16 "$raw")" | base32 | tr -d '=\n'
}

nkey_seed_text() {
  nkey_encode "$NKEY_USER_SEED_PREFIX" "$1"
}

nkey_public_text() {
  nkey_encode "$NKEY_USER_PREFIX" "$1"
}

nkey_seed_hex() {
  local text="$1" hex
  [[ "$text" =~ ^SU[A-Z2-7]{56}$ ]] || return 1
  hex="$(printf '%s======' "$text" | base32 -d 2>/dev/null | od -An -v -tx1 | tr -d ' \n')"
  [ "${#hex}" -eq 72 ] || return 1
  [ "${hex:0:4}" = "$NKEY_USER_SEED_PREFIX" ] || return 1
  [ "${hex:68:4}" = "$(nkey_crc16 "${hex:0:68}")" ] || return 1
  printf '%s' "${hex:4:64}"
}

nkey_public_hex() {
  local hex
  hex="$(nkey_binary "$NKEY_ED25519_PKCS8_PREFIX$1" \
    | openssl pkey -inform DER -pubout -outform DER 2>/dev/null \
    | od -An -v -tx1 | tr -d ' \n')"
  [ "${#hex}" -eq 88 ] || return 1
  printf '%s' "${hex:24:64}"
}

nkey_public_of() {
  local seed_hex public_hex
  seed_hex="$(nkey_seed_hex "$(tr -d '[:space:]' < "$1")")" || return 1
  public_hex="$(nkey_public_hex "$seed_hex")" || return 1
  nkey_public_text "$public_hex"
}

nkey_issue() {
  local file="$1" temp
  temp="$(umask 077 && mktemp "$file.XXXXXX")"
  if ! (umask 077 && nkey_seed_text "$(openssl rand -hex 32)" > "$temp") \
      || ! nkey_public_of "$temp" >/dev/null; then
    rm -f "$temp"
    return 1
  fi
  printf '\n' >> "$temp"
  chmod 600 "$temp"
  mv "$temp" "$file"
}

nats_permission_services() {
  sed -n 's/^ARGUS_NATS_\([A-Z0-9_]*\): {$/\1/p' "$1" | tr 'A-Z_' 'a-z-'
}

nats_permission_variable() {
  printf 'ARGUS_NATS_%s' "$(printf '%s' "$1" | tr 'a-z-' 'A-Z_')"
}

nats_service_seed() {
  local seeds="$1" service="$2"
  if [ ! -s "$seeds/$service.seed" ]; then
    nkey_issue "$seeds/$service.seed" || return 1
  fi
  chmod 600 "$seeds/$service.seed"
  if ! nkey_public_of "$seeds/$service.seed"; then
    printf 'nats-users: %s is not a valid NATS user seed; remove it and re-run to issue a new one\n' \
      "$seeds/$service.seed" >&2
    return 1
  fi
}

nats_auth_conf() {
  local permissions="$1" seeds="$2" legacy_password="$3" service public
  printf 'include ./%s\n\nauthorization {\n  users: [\n' "${permissions##*/}"
  if [ -n "$legacy_password" ]; then
    printf '    {user: "argus", password: "%s"}\n' "$legacy_password"
  fi
  for service in $(nats_permission_services "$permissions"); do
    public="$(nats_service_seed "$seeds" "$service")" || return 1
    printf '    {nkey: "%s", permissions: $%s}\n' "$public" "$(nats_permission_variable "$service")"
  done
  printf '  ]\n}\n'
}
