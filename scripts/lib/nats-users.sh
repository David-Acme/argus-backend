#!/usr/bin/env bash

NKEY_ED25519_PKCS8_PREFIX="302e020100300506032b657004220420"
NKEY_USER_SEED_PREFIX="9500"
NKEY_USER_PREFIX="a0"

NKEY_AWK='
  function hex_value(digit) {
    return index("0123456789abcdef", tolower(digit)) - 1
  }
  function byte_at(hex, at) {
    return hex_value(substr(hex, 2 * at - 1, 1)) * 16 + hex_value(substr(hex, 2 * at, 1))
  }
  function xor16(a, b,   result, bit, i) {
    result = 0
    bit = 1
    for (i = 0; i < 16; i++) {
      if (int(a / bit) % 2 != int(b / bit) % 2)
        result += bit
      bit *= 2
    }
    return result
  }
  function crc16(hex,   crc, at, round) {
    crc = 0
    for (at = 1; at <= length(hex) / 2; at++) {
      crc = xor16(crc, byte_at(hex, at) * 256)
      for (round = 0; round < 8; round++) {
        if (crc >= 32768)
          crc = xor16((crc * 2) % 65536, 4129)
        else
          crc = (crc * 2) % 65536
      }
    }
    return sprintf("%02x%02x", crc % 256, int(crc / 256))
  }
  function encode(hex, alphabet, width,   out, buffer, bits, at) {
    buffer = 0
    bits = 0
    for (at = 1; at <= length(hex) / 2; at++) {
      buffer = buffer * 256 + byte_at(hex, at)
      bits += 8
      while (bits >= width) {
        bits -= width
        out = out substr(alphabet, int(buffer / 2 ^ bits) + 1, 1)
        buffer = buffer % 2 ^ bits
      }
    }
    if (bits > 0)
      out = out substr(alphabet, buffer * 2 ^ (width - bits) + 1, 1)
    return out
  }
  function base32(hex) {
    return encode(hex, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", 5)
  }
  function base64(hex,   out) {
    out = encode(hex, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", 6)
    while (length(out) % 4)
      out = out "="
    return out
  }
  function unbase32(text,   out, buffer, bits, at, value) {
    buffer = 0
    bits = 0
    for (at = 1; at <= length(text); at++) {
      value = index("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", substr(text, at, 1)) - 1
      if (value < 0)
        return ""
      buffer = buffer * 32 + value
      bits += 5
      if (bits >= 8) {
        bits -= 8
        out = out sprintf("%02x", int(buffer / 2 ^ bits))
        buffer = buffer % 2 ^ bits
      }
    }
    return out
  }
'

nkey_encode() {
  awk -v prefix="$1" "$NKEY_AWK"'
    { hex = hex $0 }
    END {
      gsub(/[^0-9A-Fa-f]/, "", hex)
      if (hex == "")
        exit 1
      raw = prefix tolower(hex)
      printf "%s", base32(raw crc16(raw))
    }'
}

nkey_seed_payload() {
  awk -v prefix="$NKEY_USER_SEED_PREFIX" "$NKEY_AWK"'
    { text = text $0 }
    END {
      gsub(/[[:space:]]/, "", text)
      if (text !~ /^SU[A-Z2-7]+$/ || length(text) != 58)
        exit 1
      hex = unbase32(text)
      if (length(hex) != 72 || substr(hex, 1, 4) != prefix)
        exit 1
      if (substr(hex, 69, 4) != crc16(substr(hex, 1, 68)))
        exit 1
      print substr(hex, 5, 64)
    }'
}

nkey_private_pem() {
  awk -v prefix="$NKEY_ED25519_PKCS8_PREFIX" "$NKEY_AWK"'
    { hex = hex $0 }
    END {
      if (hex !~ /^[0-9a-f]+$/ || length(hex) != 64)
        exit 1
      print "-----BEGIN PRIVATE KEY-----"
      print base64(prefix hex)
      print "-----END PRIVATE KEY-----"
    }'
}

nkey_public_of() {
  nkey_seed_payload < "$1" \
    | nkey_private_pem \
    | openssl pkey -pubout -outform DER 2>/dev/null \
    | od -An -v -tx1 \
    | awk '
        { hex = hex $0 }
        END {
          gsub(/[^0-9a-f]/, "", hex)
          if (length(hex) != 88)
            exit 1
          print substr(hex, 25, 64)
        }' \
    | nkey_encode "$NKEY_USER_PREFIX"
}

nkey_issue() {
  local file="$1" temp
  temp="$(umask 077 && mktemp "$file.XXXXXX")"
  if ! (umask 077 && openssl rand -hex 32 | nkey_encode "$NKEY_USER_SEED_PREFIX" > "$temp") \
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
  local permissions="$1" seeds="$2" service public
  printf 'include ./%s\n\nauthorization {\n  users: [\n' "${permissions##*/}"
  for service in $(nats_permission_services "$permissions"); do
    public="$(nats_service_seed "$seeds" "$service")" || return 1
    printf '    {nkey: "%s", permissions: $%s}\n' "$public" "$(nats_permission_variable "$service")"
  done
  printf '  ]\n}\n'
}
