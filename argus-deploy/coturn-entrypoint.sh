#!/bin/sh
set -eu

secret_file="${ARGUS_TURN_SECRET_FILE:-/run/secrets/turn_secret}"
conf=/tmp/turnserver.conf

if [ ! -s "$secret_file" ]; then
  echo "coturn: no static auth secret at $secret_file; run scripts/provision-host.sh --turn-host <name>" >&2
  exit 1
fi

umask 077
{
  printf 'listening-port=%s\n' "${ARGUS_TURN_PORT:-3478}"
  printf 'tls-listening-port=%s\n' "${ARGUS_TURN_TLS_PORT:-5349}"
  printf 'min-port=%s\n' "${ARGUS_TURN_MIN_PORT:-49160}"
  printf 'max-port=%s\n' "${ARGUS_TURN_MAX_PORT:-49199}"
  printf 'realm=%s\n' "${ARGUS_TURN_REALM:-argus}"
  printf 'use-auth-secret\n'
  printf 'static-auth-secret=%s\n' "$(head -n 1 "$secret_file" | tr -d '\r\n')"
  printf 'cert=%s\n' "${ARGUS_TLS_CERT:-/etc/argus/certs/server.pem}"
  printf 'pkey=%s\n' "${ARGUS_TLS_KEY:-/etc/argus/certs/server.key}"
  printf 'fingerprint\n'
  printf 'no-cli\n'
  printf 'no-tlsv1\n'
  printf 'no-tlsv1_1\n'
  printf 'no-multicast-peers\n'
  printf 'stale-nonce=600\n'
  printf 'total-quota=%s\n' "${ARGUS_TURN_TOTAL_QUOTA:-32}"
  printf 'user-quota=%s\n' "${ARGUS_TURN_USER_QUOTA:-8}"
  printf 'log-file=stdout\n'
  printf 'pidfile=/tmp/turnserver.pid\n'
  printf 'simple-log\n'
  for range in \
      0.0.0.0-0.255.255.255 \
      10.0.0.0-10.255.255.255 \
      100.64.0.0-100.127.255.255 \
      127.0.0.0-127.255.255.255 \
      169.254.0.0-169.254.255.255 \
      172.16.0.0-172.31.255.255 \
      192.0.0.0-192.0.0.255 \
      192.168.0.0-192.168.255.255 \
      198.18.0.0-198.19.255.255 \
      240.0.0.0-255.255.255.255 \
      ::1 \
      fc00::-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff \
      fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff; do
    printf 'denied-peer-ip=%s\n' "$range"
  done
  for peer in $(printf '%s' "${ARGUS_TURN_ALLOWED_PEERS:-}" | tr ',' ' '); do
    printf 'allowed-peer-ip=%s\n' "$peer"
  done
  if [ -n "${ARGUS_TURN_EXTERNAL_IP:-}" ]; then
    printf 'external-ip=%s\n' "$ARGUS_TURN_EXTERNAL_IP"
  fi
} > "$conf"

exec turnserver -c "$conf"
