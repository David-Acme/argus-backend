#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/boot-proof.sh [--config-only] <service>

Boots services/<service>/build/dev/argus-<service> once with its mutual TLS
listener on, proves it, and stops it. One line per check; exit 0 only when
every check passes.

- template: the service's config.toml.example is read from HEAD with git,
  never from a config.toml of the tree, of ~/argus-demo or of argus-cutover.
- pki: a scratch internal CA and service certificates, issued by
  scripts/lib/internal-pki.sh, the generator internal-tls-test.sh uses.
- config: config.toml written by scripts/lib/boot_proof.py. Every
  listener gets a random free loopback port that is neither 4222 nor a port
  of the port map (scripts/port-map-test.sh --list); [internal_tls] reads the
  scratch material; the databases live in the scratch directory; every peer
  points at one closed loopback port; NATS and mDNS are off; secrets and
  caller credentials are random.
- health: /health answers within 30 s and reports internalTls.loaded true.
- tls-with-certificate: grpc.health.v1.Health/Check over the mutual TLS port,
  presenting another service's scratch certificate, gets a status from the
  server: SERVING where the service serves grpc.health.v1, any other status
  the server sends (UNIMPLEMENTED included) where it does not. Either shows
  that the mutual handshake completed and the request reached the server.
- tls-without-certificate: the same call without a client certificate fails
  at TLS: UNAVAILABLE from the transport with a handshake error. A status from
  the server fails the check.
  Origin: grpcurl -v prints the response headers and trailers it received. A
  status comes from the server when either holds at least one metadata line
  (a gRPC server always sends content-type: application/grpc); it comes from
  the transport when both are empty or the dial failed. Each line names the
  status and its origin.
- stop: after SIGTERM the process is gone within 10 s and none of its ports
  still listens (ss).

The scratch directory comes from mktemp under ARGUS_BOOT_PROOF_DIR (default
${XDG_CACHE_HOME:-~/.cache}/argus-boot-proof), which may not be under /tmp,
and is removed on exit, the process killed if it is still running.
grpcurl is taken from ARGUS_GRPCURL, the PATH or ~/go/bin. --config-only
writes the config, prints it with its secrets masked and starts nothing.
USAGE
}

config_only=0
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --config-only) config_only=1; shift ;;
esac
[ "$#" -eq 1 ] || { usage >&2; exit 2; }
service="$1"
case "$service" in
  ""|*[!a-z0-9-]*) usage >&2; exit 2 ;;
esac

base="$(realpath -m -- "${ARGUS_BOOT_PROOF_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/argus-boot-proof}")"
case "$base" in
  /tmp|/tmp/*)
    printf 'boot-proof: the scratch base %s is under /tmp\n' "$base" >&2
    exit 2
    ;;
esac
mkdir -p "$base"
scratch="$(mktemp -d "$base/$service-XXXXXX")"
pid=""

cleanup() {
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  rm -rf -- "$scratch"
}
trap cleanup EXIT

failures=0
pass() {
  printf 'boot-proof: %s: ok%s\n' "$1" "${2:+ ($2)}"
}
miss() {
  printf 'boot-proof: %s: FAIL %s\n' "$1" "$2"
  failures=$((failures + 1))
}
finish() {
  if [ "$failures" -eq 0 ]; then
    printf 'boot-proof: %s: every check passed\n' "$service"
    exit 0
  fi
  if [ -s "$scratch/service.log" ]; then
    printf 'boot-proof: the last lines of argus-%s:\n' "$service" >&2
    tail -n 20 "$scratch/service.log" >&2
  fi
  printf 'boot-proof: %s: %s checks failed\n' "$service" "$failures"
  exit 1
}

listening() {
  [ -n "$(ss -ltnH "sport = :$1" 2>/dev/null)" ]
}

binary="$ROOT/services/$service/build/dev/argus-$service"
if [ "$config_only" -eq 0 ]; then
  if [ -x "$binary" ]; then
    pass binary "$binary"
  else
    miss binary "$binary is missing or not executable"
    finish
  fi
fi

template="$scratch/config.toml.example"
if git -C "$ROOT" show "HEAD:services/$service/config.toml.example" > "$template" 2>/dev/null; then
  pass template "services/$service/config.toml.example at $(git -C "$ROOT" rev-parse --short HEAD)"
else
  miss template "HEAD has no services/$service/config.toml.example"
  finish
fi

source "$ROOT/scripts/lib/common.sh"
source "$ROOT/scripts/lib/internal-pki.sh"
mkdir -p "$scratch/run" "$scratch/data" "$scratch/certs"
if ensure_internal_pki "$scratch/pki" > "$scratch/pki.log" 2>&1 && [ -s "$scratch/pki/$service/cert.pem" ]; then
  pass pki "scratch internal CA and certificates"
else
  miss pki "no scratch certificate for $service"
  finish
fi

peer=sync
[ "$service" != "$peer" ] || peer=auth

if openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$scratch/certs/server.key" -out "$scratch/certs/server.pem" -days 1 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    > "$scratch/certs.log" 2>&1; then
  chmod 600 "$scratch/certs/server.key"
else
  miss certificate "the scratch listener certificate could not be made"
  finish
fi

bash "$ROOT/scripts/port-map-test.sh" --list 2>/dev/null \
  | awk '$1 ~ /^[0-9]+$/ { print $1 }' | sort -un > "$scratch/reserved" || true
if [ "$(wc -l < "$scratch/reserved")" -lt 40 ]; then
  miss ports "the port map could not be read"
  finish
fi

if python3 -I "$ROOT/scripts/lib/boot_proof.py" config --service "$service" \
    --template "$template" --root "$ROOT" --scratch "$scratch" \
    --pki "$scratch/pki/$service" --cert "$scratch/certs/server.pem" \
    --key "$scratch/certs/server.key" --reserved "$scratch/reserved" \
    --out "$scratch/run/config.toml" --summary "$scratch/summary.json" \
    --masked "$scratch/config.masked.toml"; then
  :
else
  miss config "config.toml could not be written"
  finish
fi

summary_field() {
  python3 -I "$ROOT/scripts/lib/boot_proof.py" field --summary "$scratch/summary.json" "$1"
}

health_scheme="$(summary_field health.scheme)"
health_port="$(summary_field health.port)"
tls_port="$(summary_field tls)"
listeners="$(summary_field listeners)"
closed="$(summary_field closed)"

if [ -z "$health_port" ] || [ -z "$tls_port" ]; then
  miss config "the template declares no health listener or no [internal_tls] address"
  finish
fi
if listening "$closed"; then
  miss config "the closed peer port $closed is listening"
  finish
fi
pass config "health $health_scheme://127.0.0.1:$health_port, mutual TLS 127.0.0.1:$tls_port, listeners $listeners, peers on closed port $closed, reserved $(wc -l < "$scratch/reserved") ports and 4222 avoided"

if [ "$config_only" -eq 1 ]; then
  cat "$scratch/config.masked.toml"
  exit 0
fi

grpcurl="${ARGUS_GRPCURL:-}"
if [ -z "$grpcurl" ]; then
  grpcurl="$(command -v grpcurl 2>/dev/null || true)"
fi
if [ -z "$grpcurl" ] && [ -x "$HOME/go/bin/grpcurl" ]; then
  grpcurl="$HOME/go/bin/grpcurl"
fi
if [ -z "$grpcurl" ] || [ ! -x "$grpcurl" ]; then
  miss grpcurl "no grpcurl in ARGUS_GRPCURL, the PATH or ~/go/bin"
  finish
fi

(
  cd "$scratch/run"
  exec "$binary"
) > "$scratch/service.log" 2>&1 < /dev/null &
pid=$!

body=""
deadline=$((SECONDS + 30))
while [ "$SECONDS" -lt "$deadline" ]; do
  kill -0 "$pid" 2>/dev/null || break
  if body="$(curl -ksS --max-time 2 "$health_scheme://127.0.0.1:$health_port/health" 2>/dev/null)" && [ -n "$body" ]; then
    break
  fi
  body=""
  sleep 0.5
done

if [ -z "$body" ]; then
  if kill -0 "$pid" 2>/dev/null; then
    miss health "no answer from /health within 30 s"
  else
    miss health "argus-$service exited before /health answered"
  fi
else
  loaded="$(printf '%s' "$body" | python3 -I "$ROOT/scripts/lib/boot_proof.py" health)"
  if [ "$loaded" = true ]; then
    pass health "internalTls.loaded true"
  else
    miss health "internalTls.loaded is not true ($loaded)"
  fi
fi

health_call() {
  "$grpcurl" -v -max-time 5 -import-path "$ROOT/packages/contracts/proto" \
    -proto grpc/health/v1/health.proto -cacert "$scratch/pki/$peer/ca.pem" \
    -servername "argus-$service" "$@" -d '{}' "127.0.0.1:$tls_port" \
    grpc.health.v1.Health/Check 2>&1 || true
}

classified() {
  printf '%s' "$1" | python3 -I "$ROOT/scripts/lib/boot_proof.py" classify
}

if kill -0 "$pid" 2>/dev/null; then
  answer="$(health_call -cert "$scratch/pki/$peer/cert.pem" -key "$scratch/pki/$peer/key.pem")"
  read -r status origin serving handshake <<< "$(classified "$answer")"
  shown="status $status from the $origin"
  [ "$serving" = serving ] && shown="$shown, SERVING"
  if [ "$origin" = server ]; then
    pass tls-with-certificate "$shown, to argus-$peer's certificate"
  else
    miss tls-with-certificate "$shown: $(printf '%s' "$answer" | grep -E 'Failed to dial|Message:' | head -1 | cut -c1-160)"
  fi
  before="$(wc -c < "$scratch/service.log")"
  answer="$(health_call)"
  read -r status origin serving handshake <<< "$(classified "$answer")"
  shown="status $status from the $origin"
  refusal="$(tail -c +"$((before + 1))" "$scratch/service.log" | grep -m 1 -E 'peer did not return a certificate|Handshake failed' || true)"
  if [ "$origin" = transport ] && [ "$status" = UNAVAILABLE ] && [ -n "$refusal" ]; then
    pass tls-without-certificate "$shown; service log since the call: $(printf '%s' "$refusal" | cut -c1-160)"
  else
    miss tls-without-certificate "$shown$([ -n "$refusal" ] || printf ', no TLS refusal in the service log since the call')"
  fi
else
  miss tls-with-certificate "argus-$service is not running"
  miss tls-without-certificate "argus-$service is not running"
fi

if [ "$service" = tts ]; then
  deadline=$((SECONDS + 60))
  while [ "$SECONDS" -lt "$deadline" ] && ! grep -q "Pocket TTS loaded" "$scratch/service.log"; do
    sleep 1
  done
  if grep -q "Pocket TTS loaded" "$scratch/service.log"; then
    pass pocket-loaded "$(grep -m 1 "Pocket TTS loaded" "$scratch/service.log" | cut -c1-120)"
  else
    miss pocket-loaded "no 'Pocket TTS loaded' line within 60 s"
  fi
fi

if kill -0 "$pid" 2>/dev/null; then
  kill -TERM "$pid" 2>/dev/null || true
  deadline=$((SECONDS + 10))
  while [ "$SECONDS" -lt "$deadline" ] && kill -0 "$pid" 2>/dev/null; do
    sleep 0.2
  done
fi
still=""
for port in $listeners; do
  if listening "$port"; then
    still="$still $port"
  fi
done
if kill -0 "$pid" 2>/dev/null; then
  miss stop "argus-$service (pid $pid) is still running 10 s after SIGTERM"
elif [ -n "$still" ]; then
  miss stop "ports still listening:$still"
else
  pass stop "pid $pid gone, none of $listeners listening"
fi
wait "$pid" 2>/dev/null || true
pid=""

finish
