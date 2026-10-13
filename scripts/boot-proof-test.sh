#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/boot-proof-test.sh

Checks scripts/boot-proof.sh and scripts/lib/boot_proof.py without starting
any service:
- the config each service would boot with (boot-proof.sh --config-only), for
  every service whose config.toml.example at HEAD declares [internal_tls]:
  every listener on a free port between 20000 and 60999 that is neither 4222
  nor a port of the port map, every peer on the one closed port, [internal_tls]
  and the databases inside the scratch directory, every other absolute path
  inside the tree, NATS and mDNS off, every secret and caller credential set;
- a service whose template at HEAD declares no [internal_tls] is refused,
  with the reason;
- a scratch base under /tmp is refused;
- relative paths resolve inside the service or the tree, never outside it;
- the /health reader, on an enveloped body, guard's top-level body, a false
  and a missing internalTls;
- the gRPC status classifier, on grpcurl -v output: a server OK with SERVING,
  a server UNIMPLEMENTED, a transport UNAVAILABLE without metadata, a dial
  that ran out of time and a refused handshake;
- with grpcurl and openssl present, a real refused handshake: grpcurl without
  a client certificate against openssl s_server requiring one, classified as
  UNAVAILABLE from the transport with a handshake error.
It exits 77 when grpcurl is missing, after every other check has passed.
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) usage >&2; exit 2 ;;
esac

fail() {
  printf 'boot-proof-test: %s\n' "$*" >&2
  exit 1
}

base="$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/argus-boot-proof-test-XXXXXX")"
server_pid=""
cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
  fi
  rm -rf -- "$base"
}
trap cleanup EXIT

bash "$ROOT/scripts/port-map-test.sh" --list 2>/dev/null \
  | awk '$1 ~ /^[0-9]+$/ { print $1 }' | sort -un > "$base/reserved" || true
[ "$(wc -l < "$base/reserved")" -ge 40 ] || fail "the port map could not be read"

check_config() {
  local service="$1" out="$base/$service.toml"
  ARGUS_BOOT_PROOF_DIR="$base/runs" bash "$ROOT/scripts/boot-proof.sh" --config-only "$service" > "$out" 2> "$base/$service.err" \
    || fail "$service: --config-only failed: $(cat "$base/$service.err")"
  grep -q "^boot-proof: config: ok" "$out" || fail "$service: no config line"
  python3 -I - "$out" "$base/reserved" "$base/runs" "$service" "$ROOT" <<'PY' || fail "$service: the config breaks a rule"
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
reserved = {int(line) for line in open(sys.argv[2], encoding="utf-8") if line.strip().isdigit()}
runs = sys.argv[3]
service = sys.argv[4]
root = sys.argv[5]
summary = re.search(r"peers on closed port (\d+)", text)
if not summary:
    sys.exit("no closed port in the config line")
closed = int(summary.group(1))
section = ""
problems = []
listener_keys = {"port", "grpc_port", "health_port", "device_port", "home_port"}
for line in text.splitlines():
    header = re.match(r"^\s*\[([^\]]+)\]\s*$", line)
    if header:
        section = header.group(1)
        continue
    entry = re.match(r"^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$", line)
    if not entry:
        continue
    key, value = entry.group(1), entry.group(2).strip('"')
    if (section == "server" and key in listener_keys) or (section == service and key == "port"):
        port = int(value)
        if port == 4222 or port in reserved or not 20000 <= port <= 60999:
            problems.append(f"[{section}] {key} = {port}")
    if section in ("rpc", "internal_tls") and key == "address":
        port = int(value.rsplit(":", 1)[1])
        if not value.startswith("127.0.0.1:") or port == 4222 or port in reserved:
            problems.append(f"[{section}] address = {value}")
    if section == "internal_tls" and key == "dir" and not value.startswith(runs):
        problems.append(f"[internal_tls] dir = {value}")
    if key in ("db", "db_path") and not value.startswith(runs):
        problems.append(f"[{section}] {key} = {value}")
    if section == "nats" and key == "url" and value:
        problems.append(f"[nats] url = {value}")
    if section == "mdns" and key == "enabled" and value != "false":
        problems.append(f"[mdns] enabled = {value}")
    if section == "rpc.callers" or (section == "grpc" and key.startswith("caller_")) or \
            (section, key) in (("jwt", "secret"), ("jwt", "refresh_secret"),
                               ("device", "fingerprint_secret")):
        if value != "<masked>":
            problems.append(f"[{section}] {key} is not set")
    if value.startswith("/") and not (value.startswith(root + "/") or value.startswith(runs + "/")):
        problems.append(f"[{section}] {key} = {value} leaves the tree and the scratch directory")
    peer = re.match(r"^(?:[a-z]+://)?(?:127\.0\.0\.1|localhost|0\.0\.0\.0):(\d+)", value)
    if peer and not (section in ("rpc", "internal_tls") and key == "address") and \
            int(peer.group(1)) != closed:
        problems.append(f"[{section}] {key} = {value} is not the closed port {closed}")
if problems:
    sys.exit("; ".join(problems))
PY
}

wired=()
unwired=""
for template in "$ROOT"/services/*/config.toml.example; do
  service="${template%/config.toml.example}"
  service="${service##*/}"
  if git -C "$ROOT" show "HEAD:services/$service/config.toml.example" 2>/dev/null | grep -qx '\[internal_tls\]'; then
    wired+=("$service")
  elif [ -z "$unwired" ] && [ "$service" != tunnel ]; then
    unwired="$service"
  fi
done
[ "${#wired[@]}" -ge 1 ] || fail "no template at HEAD declares [internal_tls]"
for service in "${wired[@]}"; do
  check_config "$service"
done
if [ -n "$unwired" ]; then
  if ARGUS_BOOT_PROOF_DIR="$base/runs" bash "$ROOT/scripts/boot-proof.sh" --config-only "$unwired" > "$base/unwired.out" 2>&1; then
    fail "$unwired has no [internal_tls] at HEAD and was accepted"
  fi
  grep -q 'no \[internal_tls\] address' "$base/unwired.out" || fail "$unwired was refused without the reason"
fi
[ -z "$(ls -A "$base/runs" 2>/dev/null)" ] || fail "--config-only left a scratch directory behind"

if ARGUS_BOOT_PROOF_DIR=/tmp/argus-boot-proof-refused bash "$ROOT/scripts/boot-proof.sh" --config-only auth > "$base/tmp.out" 2>&1; then
  fail "a scratch base under /tmp was accepted"
fi
grep -q "is under /tmp" "$base/tmp.out" || fail "the /tmp refusal does not say why"

resolved() {
  python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import boot_proof; print(boot_proof.resolve_path(sys.argv[2], sys.argv[3], sys.argv[4]))' \
    "$ROOT/scripts/lib" "$1" "$ROOT" "$2"
}
[ "$(resolved database/schema.sql settings)" = "$ROOT/services/settings/database/schema.sql" ] \
  || fail "a path relative to the service was not resolved inside it"
[ "$(resolved ../../.. settings)" = "../../.." ] \
  || fail "a relative path that leaves the tree was resolved: $(resolved ../../.. settings)"

health() {
  printf '%s' "$1" | python3 -I "$ROOT/scripts/lib/boot_proof.py" health
}
[ "$(health '{"status":200,"info":{"service":"argus-auth","internalTls":{"loaded":true}},"errors":null}')" = true ] \
  || fail "an enveloped loaded section was not read"
[ "$(health '{"status":"ok","service":"argus-guard","internalTls":{"loaded":true}}')" = true ] \
  || fail "guard's top-level section was not read"
[ "$(health '{"status":200,"info":{"internalTls":{"loaded":false}},"errors":null}')" = false ] \
  || fail "a section not loaded read as loaded"
[ "$(health '{"status":200,"info":{"service":"argus-auth"},"errors":null}')" = absent ] \
  || fail "a missing section was not reported"

classify() {
  python3 -I "$ROOT/scripts/lib/boot_proof.py" classify < "$1"
}
cat > "$base/serving.out" <<'OUT'
Resolved method descriptor:
rpc Check ( .grpc.health.v1.HealthCheckRequest ) returns ( .grpc.health.v1.HealthCheckResponse );

Request metadata to send:
(empty)

Response headers received:
content-type: application/grpc

Response contents:
{
  "status": "SERVING"
}

Response trailers received:
(empty)
Sent 1 request and received 1 response
OUT
cat > "$base/unimplemented.out" <<'OUT'
Resolved method descriptor:
rpc Check ( .grpc.health.v1.HealthCheckRequest ) returns ( .grpc.health.v1.HealthCheckResponse );

Request metadata to send:
(empty)

Response headers received:
(empty)

Response trailers received:
content-type: application/grpc
Sent 1 request and received 0 responses
ERROR:
  Code: Unimplemented
  Message: unknown service grpc.health.v1.Health
OUT
cat > "$base/transport.out" <<'OUT'
Resolved method descriptor:
rpc Check ( .grpc.health.v1.HealthCheckRequest ) returns ( .grpc.health.v1.HealthCheckResponse );

Request metadata to send:
(empty)

Response headers received:
(empty)

Response trailers received:
(empty)
Sent 1 request and received 0 responses
ERROR:
  Code: Unavailable
  Message: connection error: desc = "transport: error while dialing: dial tcp 127.0.0.1:1: connect: connection refused"
OUT
cat > "$base/deadline.out" <<'OUT'
Failed to dial target host "127.0.0.1:1": context deadline exceeded
OUT
cat > "$base/refused.out" <<'OUT'
Failed to dial target host "127.0.0.1:37907": remote error: tls: certificate required
OUT
[ "$(classify "$base/serving.out")" = "OK server serving no-handshake-error" ] \
  || fail "a server SERVING answer was classified as $(classify "$base/serving.out")"
[ "$(classify "$base/unimplemented.out")" = "UNIMPLEMENTED server not-serving no-handshake-error" ] \
  || fail "a server UNIMPLEMENTED answer was classified as $(classify "$base/unimplemented.out")"
[ "$(classify "$base/transport.out")" = "UNAVAILABLE transport not-serving no-handshake-error" ] \
  || fail "a transport UNAVAILABLE was classified as $(classify "$base/transport.out")"
[ "$(classify "$base/deadline.out")" = "UNAVAILABLE transport not-serving no-handshake-error" ] \
  || fail "a dial that ran out of time was classified as $(classify "$base/deadline.out")"
[ "$(classify "$base/refused.out")" = "UNAVAILABLE transport not-serving handshake" ] \
  || fail "a refused handshake was classified as $(classify "$base/refused.out")"

grpcurl="${ARGUS_GRPCURL:-$(command -v grpcurl 2>/dev/null || true)}"
if [ -z "$grpcurl" ] && [ -x "$HOME/go/bin/grpcurl" ]; then
  grpcurl="$HOME/go/bin/grpcurl"
fi
if [ -z "$grpcurl" ] || ! command -v openssl >/dev/null 2>&1; then
  printf 'boot-proof-test: SKIPPED: the real handshake case needs grpcurl and openssl\n'
  printf 'boot-proof-test: every other check passed\n'
  exit 77
fi

(
  source "$ROOT/scripts/lib/common.sh"
  source "$ROOT/scripts/lib/internal-pki.sh"
  ensure_internal_pki "$base/pki" > "$base/pki.log" 2>&1
)
port=""
for _ in $(seq 1 50); do
  candidate="$(shuf -i 20000-60999 -n 1)"
  grep -qx "$candidate" "$base/reserved" && continue
  [ -z "$(ss -ltnH "sport = :$candidate" 2>/dev/null)" ] || continue
  port="$candidate"
  break
done
[ -n "$port" ] || fail "no free port for the handshake case"
openssl s_server -accept "$port" -alpn h2 -cert "$base/pki/auth/cert.pem" \
  -key "$base/pki/auth/key.pem" -CAfile "$base/pki/auth/ca.pem" -Verify 1 \
  -verify_return_error -quiet > "$base/s_server.log" 2>&1 < /dev/null &
server_pid=$!
for _ in $(seq 1 50); do
  [ -n "$(ss -ltnH "sport = :$port" 2>/dev/null)" ] && break
  sleep 0.1
done
"$grpcurl" -v -max-time 5 -import-path "$ROOT/packages/contracts/proto" \
  -proto grpc/health/v1/health.proto -cacert "$base/pki/sync/ca.pem" \
  -servername argus-auth -d '{}' "127.0.0.1:$port" grpc.health.v1.Health/Check \
  > "$base/real-refused.out" 2>&1 || true
[ "$(classify "$base/real-refused.out")" = "UNAVAILABLE transport not-serving handshake" ] \
  || fail "a real refused handshake was classified as $(classify "$base/real-refused.out"): $(cat "$base/real-refused.out")"

printf 'boot-proof-test: %s service configs (%s), %s, the /tmp refusal, 4 health bodies, 5 classified answers and a real refused handshake checked\n' \
  "${#wired[@]}" "${wired[*]}" "${unwired:+$unwired refused without [internal_tls]}"
