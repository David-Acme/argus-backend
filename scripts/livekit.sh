#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"

LIVEKIT_IMAGE="${ARGUS_LIVEKIT_IMAGE:-livekit/livekit-server:v1.13.7}"
TLS_IMAGE="${ARGUS_LIVEKIT_TLS_IMAGE:-nginx:1.28.3-alpine}"
RUN_DIR="${ARGUS_LIVEKIT_DIR:-$ROOT/build/livekit}"
CERTS_DIR="${ARGUS_CERTS_DIR:-$ROOT/certs}"
SYNC_CONFIG="${ARGUS_LIVEKIT_SYNC_CONFIG:-$ROOT/services/sync/config.toml}"
SANDBOX_SYNC_CONFIG="${ARGUS_STACK_DIR:-$ROOT/build/native-stack}/sync/config.toml"
LIVEKIT_MEMORY="${ARGUS_LIVEKIT_MEMORY_LIMIT:-384m}"
TLS_MEMORY="${ARGUS_LIVEKIT_TLS_MEMORY_LIMIT:-32m}"
LIVEKIT_NAME=argus-dev-livekit
TLS_NAME=argus-dev-livekit-tls

usage() {
  cat <<'USAGE'
Argus backend - LiveKit for native development.

Runs the LiveKit SFU and its TLS front as two memory-capped containers on the
host network, for the natively run services (argus-sync mints the tokens,
argus-voice joins as the agent). The deployment stack has its own pair in
argus-deploy/docker-compose.yml; this one never touches it.

  LiveKit   127.0.0.1:7880 (HTTP API + plain ws, loopback only)
            7881/tcp ICE over TCP, 7882/udp ICE (single mux port)
  TLS front 0.0.0.0:7046 (wss for the apps, the instance certificate)

The API key and secret are argus-sync's [rtc] api_key / api_secret
(generated here when empty, 0600 like the config itself). They are written
to a 0600 key file under ARGUS_LIVEKIT_DIR and copied into the sandbox's
sync config when the native stack has one.

Usage:
  livekit.sh up                     render the config, start both containers
  livekit.sh down                   stop and remove both containers
  livekit.sh status                 show the containers and LiveKit's health
  livekit.sh logs [n]               tail LiveKit's log
  livekit.sh token <room> <identity> [ttl_seconds]
                                    print a participant token for a test client

Environment:
  ARGUS_LIVEKIT_DIR             rendered config and key file (build/livekit)
  ARGUS_LIVEKIT_SYNC_CONFIG     sync config that holds the key pair
  ARGUS_CERTS_DIR               certificate directory (certs)
  ARGUS_LIVEKIT_MEMORY_LIMIT    LiveKit memory cap (384m)
  ARGUS_LIVEKIT_TLS_MEMORY_LIMIT  TLS front memory cap (32m)
USAGE
}

ensure_keys() {
  [ -f "$SYNC_CONFIG" ] || { err "missing $SYNC_CONFIG (run scripts/setup.sh)"; exit 1; }
  toml_key_exists "$SYNC_CONFIG" rtc api_key || replace_toml_value rtc api_key "" "$SYNC_CONFIG"
  ensure_livekit_key_pair "$SYNC_CONFIG"
  if [ -f "$SANDBOX_SYNC_CONFIG" ]; then
    replace_toml_value rtc api_key "$(toml_value "$SYNC_CONFIG" rtc api_key)" "$SANDBOX_SYNC_CONFIG"
    replace_toml_value rtc api_secret "$(toml_value "$SYNC_CONFIG" rtc api_secret)" "$SANDBOX_SYNC_CONFIG"
  fi
  write_livekit_keys "$SYNC_CONFIG" "$RUN_DIR/keys.yaml"
}

render_config() {
  sed -e '/^  - 172\.19\.0\.1$/d' "$ROOT/argus-deploy/livekit.yaml.example" > "$RUN_DIR/livekit.yaml"
}

running() {
  [ -n "$(docker ps -q --filter "name=^$1\$")" ]
}

cmd_up() {
  need_cmd docker
  need_cmd openssl
  [ -r "$CERTS_DIR/server.pem" ] && [ -r "$CERTS_DIR/server.key" ] || {
    err "missing $CERTS_DIR/server.pem / server.key (run scripts/setup.sh)"
    exit 1
  }
  ensure_keys
  render_config
  docker rm -f "$LIVEKIT_NAME" "$TLS_NAME" >/dev/null 2>&1 || true
  docker run -d --name "$LIVEKIT_NAME" --network host \
    --user "$(id -u):$(id -g)" \
    --memory "$LIVEKIT_MEMORY" --memory-swap "$LIVEKIT_MEMORY" --cpus 1 \
    --pids-limit 256 --restart unless-stopped \
    -v "$RUN_DIR/livekit.yaml:/etc/livekit/livekit.yaml:ro" \
    -v "$RUN_DIR/keys.yaml:/etc/livekit/keys.yaml:ro" \
    "$LIVEKIT_IMAGE" --config /etc/livekit/livekit.yaml >/dev/null
  docker run -d --name "$TLS_NAME" --network host \
    --user "$(id -u):$(id -g)" \
    --memory "$TLS_MEMORY" --memory-swap "$TLS_MEMORY" --cpus 0.25 \
    --pids-limit 32 --restart unless-stopped \
    -v "$ROOT/argus-deploy/livekit-tls.conf:/etc/nginx/nginx.conf:ro" \
    -v "$CERTS_DIR:/etc/argus/certs:ro" \
    --entrypoint nginx "$TLS_IMAGE" -g 'daemon off;' >/dev/null
  local waited=0
  until curl -fs http://127.0.0.1:7880/ >/dev/null 2>&1; do
    waited=$((waited + 1))
    [ "$waited" -le 30 ] || { err "LiveKit did not answer on 127.0.0.1:7880"; docker logs --tail 30 "$LIVEKIT_NAME" >&2; exit 1; }
    sleep 1
  done
  waited=0
  until curl -kfs https://127.0.0.1:7046/ >/dev/null 2>&1; do
    waited=$((waited + 1))
    [ "$waited" -le 15 ] || { err "TLS front did not answer on 7046"; docker logs --tail 30 "$TLS_NAME" >&2; exit 1; }
    sleep 1
  done
  log "LiveKit is up: ws://127.0.0.1:7880 (services), wss://<host>:7046 (apps)."
}

cmd_down() {
  docker rm -f "$LIVEKIT_NAME" "$TLS_NAME" >/dev/null 2>&1 || true
  log "LiveKit stopped."
}

cmd_status() {
  local name
  for name in "$LIVEKIT_NAME" "$TLS_NAME"; do
    if running "$name"; then
      printf '%-24s up   %s\n' "$name" "$(docker stats --no-stream --format '{{.MemUsage}}' "$name")"
    else
      printf '%-24s down\n' "$name"
    fi
  done
  if curl -fs http://127.0.0.1:7880/ >/dev/null 2>&1; then
    printf 'livekit http             ok\n'
  else
    printf 'livekit http             unreachable\n'
  fi
}

cmd_token() {
  [ $# -ge 2 ] || { usage; exit 2; }
  local room="$1" identity="$2" ttl="${3:-3600}"
  local key secret
  key="$(toml_value "$SYNC_CONFIG" rtc api_key)"
  secret="$(toml_value "$SYNC_CONFIG" rtc api_secret)"
  [ -n "$key" ] && [ -n "$secret" ] || { err "no [rtc] key pair in $SYNC_CONFIG; run livekit.sh up"; exit 1; }
  LK_KEY="$key" LK_SECRET="$secret" LK_ROOM="$room" LK_IDENTITY="$identity" LK_TTL="$ttl" python3 - <<'PY'
import base64, hashlib, hmac, json, os, time

def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()

now = int(time.time())
claims = {
    "iss": os.environ["LK_KEY"],
    "sub": os.environ["LK_IDENTITY"],
    "nbf": now - 5,
    "exp": now + int(os.environ["LK_TTL"]),
    "video": {
        "roomJoin": True,
        "room": os.environ["LK_ROOM"],
        "canPublish": True,
        "canSubscribe": True,
        "canPublishData": True,
    },
}
header = b64(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64(json.dumps(claims, separators=(",", ":")).encode())
signature = hmac.new(os.environ["LK_SECRET"].encode(), f"{header}.{payload}".encode(), hashlib.sha256).digest()
print(f"{header}.{payload}.{b64(signature)}")
PY
}

main() {
  case "${1:-}" in
    up) cmd_up ;;
    down) cmd_down ;;
    status) cmd_status ;;
    logs) docker logs --tail "${2:-80}" "$LIVEKIT_NAME" ;;
    token) shift; cmd_token "$@" ;;
    -h | --help | help | "") usage ;;
    *) usage; exit 2 ;;
  esac
}

main "$@"
