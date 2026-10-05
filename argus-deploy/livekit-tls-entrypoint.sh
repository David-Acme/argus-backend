#!/bin/sh
set -eu

cert="${ARGUS_TLS_CERT:-/etc/argus/certs/server.pem}"
interval="${ARGUS_CERT_POLL_SECONDS:-60}"

stamp() {
  stat -c '%Y:%i' "$cert" 2>/dev/null || echo missing
}

nginx -g 'daemon off;' &
child=$!

stop() {
  kill -TERM "$child" 2>/dev/null || true
  wait "$child" 2>/dev/null || true
  exit 0
}
trap stop TERM INT

last=$(stamp)
while kill -0 "$child" 2>/dev/null; do
  sleep "$interval" &
  wait $! || true
  now=$(stamp)
  [ "$now" = "$last" ] && continue
  if nginx -t -q 2>/dev/null && nginx -s reload; then
    echo "livekit-tls: certificate changed, nginx reloaded" >&2
    last=$now
  fi
done
wait "$child"
