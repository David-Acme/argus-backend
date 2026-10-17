#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib/nats-users.sh"

usage() {
  cat <<'USAGE'
usage: scripts/deploy-security-check-test.sh

Runs scripts/deploy-security-check.sh against a fabricated deploy: a scratch
internal CA, one config per service, real NATS seeds and an auth.conf from the
provisioning helper, a mutual TLS listener per service and stub object store
and broker monitor, all on free high ports, never on the owner's stack. With
--verbose it prints each check run's own lines before its verdict. The
fabricated deploy is green, and every property the check claims names its
counterexample: a plaintext listener, a plain target, the shared NATS user,
a foreign key, a certificate of another service and a credentials file that is
not bucket-scoped.
Exit 77 without openssl or python3.
USAGE
}

VERBOSE=0
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --verbose) VERBOSE=1 ;;
  "") ;;
  *) usage >&2; exit 2 ;;
esac

command -v openssl >/dev/null 2>&1 || { printf 'deploy-security-check-test: skipped, openssl is missing\n'; exit 77; }
command -v python3 >/dev/null 2>&1 || { printf 'deploy-security-check-test: skipped, python3 is missing\n'; exit 77; }

TEST_TMP="$(mktemp -d)"
trap 'kill $(cat "$TEST_TMP/pids" 2>/dev/null) 2>/dev/null || true; rm -rf "$TEST_TMP"' EXIT

DEPLOY="$TEST_TMP/deploy"
DATA="$TEST_TMP/data"
mkdir -p "$DEPLOY" "$DATA/certs/internal" "$DATA/nats/seeds" "$DATA/rustfs/accounts"

fail() {
  local log
  for log in "$TEST_TMP"/*.log; do
    [ -f "$log" ] || continue
    printf -- '--- %s\n' "${log##*/}" >&2
    tail -n 5 "$log" >&2
  done
  printf 'deploy-security-check-test: %s\n' "$*" >&2
  exit 1
}

issued_ports=""
free_port() {
  local candidate
  for _ in $(seq 1 200); do
    candidate=$(( 30000 + RANDOM % 20000 ))
    case " $issued_ports " in
      *" $candidate "*) continue ;;
    esac
    if ! (exec 3<>"/dev/tcp/127.0.0.1/$candidate") 2>/dev/null; then
      issued_ports="$issued_ports $candidate"
      printf '%s' "$candidate"
      return 0
    fi
  done
  fail "no free port"
}

pids=""
start() {
  local binary="$1" name
  name="$(printf '%s' "$1" | xargs -n1 basename | tail -n 1)"
  "$@" > "$TEST_TMP/$name-$(printf '%s' "$pids" | wc -w).log" 2>&1 &
  pids="$pids $!"
  printf '%s\n' "$pids" > "$TEST_TMP/pids"
}

wait_port() {
  local port="$1"
  for _ in $(seq 1 200); do
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}

start_listener() {
  local service="$1" attempt port
  for attempt in 1 2 3; do
    port="$(free_port)"
    start openssl s_server -accept "$port" -naccept 60 -quiet \
      -cert "$DATA/certs/internal/$service/cert.pem" -key "$DATA/certs/internal/$service/key.pem" \
      -CAfile "$DATA/certs/internal/$service/ca.pem" -Verify 1
    if wait_port "$port"; then
      tls_port["$service"]="$port"
      return 0
    fi
    kill "$(printf '%s' "$pids" | awk '{ print $NF }')" 2>/dev/null || true
    sleep 0.2
  done
  for log in "$TEST_TMP"/openssl-*.log; do cat "$log" >&2; done
  fail "no mutual TLS listener for $service"
}

make_ca() {
  openssl ecparam -name prime256v1 -genkey -noout -out "$DATA/certs/internal/ca.key" 2>/dev/null
  openssl req -x509 -new -key "$DATA/certs/internal/ca.key" -sha256 -days 2 \
    -subj "/O=Argus/OU=internal-v1/CN=Argus internal CA check" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$DATA/certs/internal/ca.pem" 2>/dev/null
  local service
  for service in "$@"; do
    mkdir -p "$DATA/certs/internal/$service"
    openssl ecparam -name prime256v1 -genkey -noout -out "$DATA/certs/internal/$service/key.pem" 2>/dev/null
    openssl req -new -key "$DATA/certs/internal/$service/key.pem" \
      -subj "/O=Argus/OU=internal-v1/CN=argus-$service" -out "$TEST_TMP/$service.csr" 2>/dev/null
    printf 'subjectAltName=DNS:argus-%s,DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth,clientAuth\n' \
      "$service" > "$TEST_TMP/$service.ext"
    openssl x509 -req -in "$TEST_TMP/$service.csr" -CA "$DATA/certs/internal/ca.pem" \
      -CAkey "$DATA/certs/internal/ca.key" -set_serial "0x$(openssl rand -hex 16)" -sha256 -days 2 \
      -extfile "$TEST_TMP/$service.ext" -out "$DATA/certs/internal/$service/cert.pem" 2>/dev/null
    cp "$DATA/certs/internal/ca.pem" "$DATA/certs/internal/$service/ca.pem"
  done
}

make_seed() {
  nkey_issue "$DATA/nats/seeds/$1.seed" || fail "no seed for $1"
  nkey_public_of "$DATA/nats/seeds/$1.seed"
}

write_config() {
  local service="$1" tls_port="$2" kind="$3"
  local config="$DEPLOY/config.$service.toml" mode=none

  if [ "$service" = identity ] || [ "$service" = camera ]; then
    mode=s3
  fi
  {
    printf '[server]\nhost = "127.0.0.1"\n'
    case "$service" in
      llm) printf 'port = %s\n' "$(( tls_port - 400 ))" ;;
    esac
    if [ "$kind" = plain ]; then
      case "$service" in
        llm) printf '\n[rpc]\naddress = "0.0.0.0:%s"\n' "$(( tls_port - 200 ))" ;;
        *) printf 'grpc_port = %s\n' "$(( tls_port - 200 ))" ;;
      esac
    fi
    printf '\n[internal_tls]\ndir = "%s/certs/internal/%s"\naddress = "127.0.0.1:%s"\n' "$DATA" "$service" "$tls_port"
    if [ "$kind" = plain_target ]; then
      printf '\n[identity]\ntarget = "127.0.0.1:%s"\ncredential = "x"\n' "$tls_port"
    else
      printf '\n[identity]\ntarget = "tls:127.0.0.1:%s"\ncredential = "x"\n' "$tls_port"
    fi
    printf '\n[nats]\nurl = "nats://127.0.0.1:%s"\nnkey = "%s"\nseed_file = "seeds/%s.seed"\n' \
      "$NATS_PORT" "${nkey[$service]}" "$service"
    if [ "$kind" = shared_user ]; then
      printf 'user = "argus"\npassword = "shared"\n'
    fi
    printf '\n[storage]\nmode = "%s"\n' "$mode"
    if [ "$mode" = s3 ]; then
      printf '\n[storage.s3]\nendpoint = "http://127.0.0.1:%s"\nregion = "us-east-1"\nbucket = "argus-check"\n' "${s3_port[$service]}"
      if [ "$kind" = s3_shared_key ]; then
        printf 'access_key = "SHARED"\nsecret_key = "SHARED"\n'
      else
        printf 'credentials_file = "%s/rustfs/accounts/%s/credentials.json"\n' "$DATA" "$service"
      fi
    fi
  } > "$config"
}

cat > "$TEST_TMP/stub.py" <<'PY'
import http.server, sys, urllib.parse

mode = sys.argv[1]
port = int(sys.argv[2])
state = sys.argv[3]

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if mode == "s3":
            with open(state) as source:
                allowed = [line.strip() for line in source if line.strip()]
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            prefix = query.get("prefix", [""])[0]
            ok = bool(query.get("list-type")) and prefix in allowed
            with open(state + ".log", "a") as log:
                log.write(self.path + " " + str(200 if ok else 403) + "\n")
            body = b""
        else:
            with open(state, "rb") as source:
                body = source.read()
            ok = True
        self.send_response(200 if ok else 403)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY

SERVICES=(identity camera auth llm)
declare -A nkey=()
declare -A tls_port=()
declare -A s3_port=()
NATS_PORT="$(free_port)"
MONITOR_PORT="$(free_port)"

make_ca "${SERVICES[@]}"
for service in "${SERVICES[@]}"; do
  nkey["$service"]="$(make_seed "$service")"
  tls_port["$service"]="$(free_port)"
  s3_port["$service"]="$(free_port)"
done

cat > "$TEST_TMP/permissions.conf" <<'CONF'
ARGUS_NATS_IDENTITY: {
  publish: { allow: ["x"] }
  subscribe: { allow: ["y"] }
}
ARGUS_NATS_CAMERA: {
  publish: { allow: ["x"] }
  subscribe: { allow: ["y"] }
}
ARGUS_NATS_AUTH: {
  publish: { allow: ["x"] }
  subscribe: { allow: ["y"] }
}
ARGUS_NATS_LLM: {
  publish: { allow: ["x"] }
  subscribe: { allow: ["y"] }
}
CONF
nats_auth_conf "$TEST_TMP/permissions.conf" "$DATA/nats/seeds" > "$DATA/nats/auth.conf"

for service in "${SERVICES[@]}"; do
  mkdir -p "$DATA/rustfs/accounts/$service"
  printf '{"credentials":{"accessKey":"AK%s","secretKey":"SK%s"}}\n' "$service" "$service" \
    > "$DATA/rustfs/accounts/$service/credentials.json"
done

printf 'portraits/\nfaces/\n' > "$TEST_TMP/allowed-identity"
printf 'cameras/\n' > "$TEST_TMP/allowed-camera"
for service in identity camera; do
  : > "$TEST_TMP/allowed-$service.log"
  start python3 -I "$TEST_TMP/stub.py" s3 "${s3_port[$service]}" "$TEST_TMP/allowed-$service"
  wait_port "${s3_port[$service]}"
done

{
  printf '{"server_id":"stub","connections":['
  separator=""
  index=0
  for service in "${SERVICES[@]}"; do
    index=$(( index + 1 ))
    printf '%s{"cid":%s,"authorized_user":"%s","ip":"127.0.0.1"}' "$separator" "$index" "${nkey[$service]}"
    separator=","
  done
  printf ']}\n'
} > "$TEST_TMP/connz.json"
start python3 -I "$TEST_TMP/stub.py" connz "$MONITOR_PORT" "$TEST_TMP/connz.json"
wait_port "$MONITOR_PORT"

check() {
  local output code=0
  output="$("$ROOT/scripts/deploy-security-check.sh" --deploy-dir "$DEPLOY" --data-dir "$DATA" \
    --no-docker "$@" 2>&1)" || code=$?
  [ "$VERBOSE" -eq 0 ] || printf '%s\n' "$output" >&2
  printf '%s\n' "$output"
  printf 'exit=%s\n' "$code"
}

expect_green() {
  local name="$1"
  shift
  local output reds greens
  output="$(check "$@")" || true
  grep -q '^exit=0$' <<<"$output" || fail "$name: the fabricated deploy is not green: $(awk '$3 == "red"' <<<"$output" | head -n 3 | tr '\n' ';')"
  reds="$(awk '$3 == "red" { count++ } END { print count + 0 }' <<<"$output")"
  greens="$(awk '$3 == "green" { count++ } END { print count + 0 }' <<<"$output")"
  [ "$reds" = 0 ] || fail "$name: $reds red line(s)"
  printf '%s: green, %s line(s)\n' "$name" "$greens"
}

expect_red() {
  local name="$1" reason="$2"
  shift 2
  local output
  output="$(check "$@")" || true
  grep -q '^exit=[1-9]' <<<"$output" || fail "$name: accepted"
  grep -q "$reason" <<<"$output" || fail "$name: red without '$reason': $(awk '$3 == "red"' <<<"$output" | tr '\n' ' ')"
  printf '%s: red\n' "$name"
}

for service in "${SERVICES[@]}"; do
  start_listener "$service"
done
for service in "${SERVICES[@]}"; do
  write_config "$service" "${tls_port[$service]}" green
done

expect_green "static" --no-probe
expect_green "probed" --monitor "http://127.0.0.1:$MONITOR_PORT"

write_config identity "${tls_port[identity]}" plain
expect_red "a plaintext listener" 'declares the plaintext listener' --no-probe
write_config identity "${tls_port[identity]}" green

write_config llm "${tls_port[llm]}" plain
expect_red "a plaintext rpc address" 'declares the plaintext listener' --no-probe
write_config llm "${tls_port[llm]}" green

write_config auth "${tls_port[auth]}" plain_target
expect_red "a plain target" 'not on tls:' --no-probe
write_config auth "${tls_port[auth]}" green

write_config camera "${tls_port[camera]}" shared_user
expect_red "the shared nats user" 'names the shared argus user' --no-probe
write_config camera "${tls_port[camera]}" green

write_config camera "${tls_port[camera]}" s3_shared_key
expect_red "the shared s3 key" 'carries the shared access_key' --no-probe
write_config camera "${tls_port[camera]}" green

cp "$DATA/nats/auth.conf" "$TEST_TMP/auth.conf"
printf '    {user: "argus", password: "shared"}\n' >> "$DATA/nats/auth.conf"
expect_red "the shared user in auth.conf" 'auth.conf still emits the shared argus user' --no-probe
cp "$TEST_TMP/auth.conf" "$DATA/nats/auth.conf"

cp "$DATA/nats/seeds/identity.seed" "$TEST_TMP/identity.seed"
nkey_issue "$DATA/nats/seeds/identity.seed"
expect_red "a foreign nkey" 'holds the key' --no-probe
cp "$TEST_TMP/identity.seed" "$DATA/nats/seeds/identity.seed"

mv "$DATA/rustfs/accounts/camera/credentials.json" "$TEST_TMP/camera.json"
expect_red "a missing account pair" 'no account pair' --no-probe
mv "$TEST_TMP/camera.json" "$DATA/rustfs/accounts/camera/credentials.json"

printf 'cameras/\nportraits/\n' > "$TEST_TMP/allowed-camera"
expect_red "unscoped credentials" 'not scoped to' --monitor "http://127.0.0.1:$MONITOR_PORT"
printf 'cameras/\n' > "$TEST_TMP/allowed-camera"

cp "$DATA/certs/internal/auth/cert.pem" "$TEST_TMP/auth.pem"
cp "$DATA/certs/internal/auth/key.pem" "$TEST_TMP/auth.key"
cp "$DATA/certs/internal/camera/cert.pem" "$DATA/certs/internal/auth/cert.pem"
cp "$DATA/certs/internal/camera/key.pem" "$DATA/certs/internal/auth/key.pem"
expect_red "a certificate of another service" 'names CN=argus-camera' --no-probe
cp "$TEST_TMP/auth.pem" "$DATA/certs/internal/auth/cert.pem"
cp "$TEST_TMP/auth.key" "$DATA/certs/internal/auth/key.pem"

printf '{"server_id":"stub","connections":[{"cid":1,"authorized_user":"argus","ip":"127.0.0.1"}]}\n' \
  > "$TEST_TMP/connz.json"
expect_red "a client on the shared user" 'connected as the shared argus user' --monitor "http://127.0.0.1:$MONITOR_PORT"

printf '{"server_id":"stub","connections":[{"cid":1,"authorized_user":"UDOTHER","ip":"127.0.0.1"}]}\n' \
  > "$TEST_TMP/connz.json"
expect_red "no connection of its own" 'presents its own key' --monitor "http://127.0.0.1:$MONITOR_PORT"

printf '{"server_id":"stub","connections":[{"cid":1,"authorized_user":"%s","ip":"127.0.0.1"},{"cid":2,"authorized_user":"UDSTRANGER","ip":"127.0.0.1"}]}\n' \
  "${nkey[identity]}" > "$TEST_TMP/connz.json"
expect_red "a stranger's key" "which is no configured service's key" --monitor "http://127.0.0.1:$MONITOR_PORT"

kill $(cat "$TEST_TMP/pids") 2>/dev/null || true
sleep 0.5
expect_red "a stopped stack" 'nothing answers on 127.0.0.1' --monitor "http://127.0.0.1:$MONITOR_PORT"

printf 'deploy-security-check-test: the fabricated deploy is green and every property names its counterexample\n'
