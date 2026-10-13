#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURES="$ROOT/scripts/fixtures/nats-users-check"

usage() {
  cat <<'USAGE'
usage: scripts/nats-users-check-test.sh [--live]

Runs scripts/nats-users-check.sh over /connz?auth=1 answers recorded from the
stack's nats image (scripts/fixtures/nats-users-check: three services, auth,
camera and sync, in permissions.conf and auth.conf) and checks the verdict and
the exit status of each: every service on its own key, camera missing, a key
auth.conf does not record, a connection on the shared user, and a broker
without authorization whose connections carry no authorized_user. It also
checks that a deployed permissions row nobody connects for is missing and
that a connection never adds an expected service.

The deployed services come from --services in the recorded cases. Over a
five-row permissions file (permissions-tunnel.conf: auth, camera, sync, relay
and tunnel), a stack without the tunnel profile passes with relay and tunnel
not deployed, and a stack with the profile and no relay connection fails. A
stub docker stands in for docker compose config --services: the gate reads
its list when --services is absent, exits 2 when it fails, and exits 2 when
the stack deploys no row.

--live also starts a throwaway broker from the image argus-deploy/docker-compose.yml
names, as the container argus-gatecheck-nats-<random> on random loopback ports
(never 4222), with no volume, removed with --rm and by a trap. One client
connects with a freshly issued nkey, the test checks that the monitor's
authorized_user is that key's public key and that the gate counts the
connection as the service's own, and at the end it checks that the container
no longer exists.
USAGE
}

live=0
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --live) live=1 ;;
  "") ;;
  *) usage >&2; exit 2 ;;
esac

TEST_TMP="$(mktemp -d)"
container=""
cleanup() {
  exec 3>&- 2>/dev/null || true
  if [ -n "$container" ]; then
    docker rm -f "$container" > /dev/null 2>&1 || true
  fi
  rm -rf "$TEST_TMP"
}
trap cleanup EXIT

fail() {
  printf 'nats-users-check-test: %s\n' "$*" >&2
  exit 1
}

mkdir -p "$TEST_TMP/bin"
printf '#!/usr/bin/env bash\ncat "$ARGUS_GATE_CONNZ"\n' > "$TEST_TMP/bin/curl"
chmod 700 "$TEST_TMP/bin/curl"

printf '%s\n' '#!/usr/bin/env bash' \
  'printf "%s\n" "$*" >> "$ARGUS_GATE_DOCKER_LOG"' \
  '[ "${ARGUS_GATE_DOCKER_FAIL:-0}" = 0 ] || { echo "no configuration file provided: not found" >&2; exit 1; }' \
  'printf "nats\nrustfs\nargus-auth\nargus-camera\nargus-sync\n"' \
  'case ",${COMPOSE_PROFILES:-}," in *,tunnel,*) printf "argus-relay\nargus-tunnel-client\n" ;; esac' \
  > "$TEST_TMP/bin/docker"
chmod 700 "$TEST_TMP/bin/docker"

THREE="nats rustfs argus-auth argus-camera argus-sync"

gate() {
  local answer="$1" permissions="$2" auth="$3" services="$4"
  local arguments=(--monitor http://recorded --auth-conf "$auth" --permissions "$permissions")
  [ "$services" = compose ] || arguments+=(--services "$services")
  set +e
  PATH="$TEST_TMP/bin:$PATH" ARGUS_GATE_CONNZ="$answer" ARGUS_GATE_DOCKER_LOG="$TEST_TMP/docker.log" \
    "$ROOT/scripts/nats-users-check.sh" "${arguments[@]}" > "$TEST_TMP/out" 2>&1
  status=$?
  set -e
}

check_output() {
  local name="$1" wanted="$2" pattern
  shift 2
  [ "$status" = "$wanted" ] \
    || fail "$name: exit $status, expected $wanted: $(tail -n 1 "$TEST_TMP/out")"
  for pattern in "$@"; do
    grep -Eq -- "$pattern" "$TEST_TMP/out" || fail "$name: no line matches /$pattern/ in: $(cat "$TEST_TMP/out")"
  done
}

expect() {
  local answer="$1" wanted="$2"
  shift 2
  gate "$FIXTURES/$answer.json" "$FIXTURES/permissions.conf" "$FIXTURES/auth.conf" "$THREE"
  check_output "$answer" "$wanted" "$@"
}

service_rows() {
  awk '/^service / { on = 1; next } on && NF == 0 { exit } on { print $1 }' "$TEST_TMP/out" | tr '\n' ' '
}

key_of() {
  sed -n "s/^ *{nkey: \"\(U[A-Z2-7]*\)\", permissions: \\\$ARGUS_NATS_$1}\$/\1/p" "$FIXTURES/auth.conf"
}

[ -n "$(key_of AUTH)" ] && [ -n "$(key_of CAMERA)" ] && [ -n "$(key_of SYNC)" ] \
  || fail "the recorded auth.conf does not hold a key for auth, camera and sync"
grep -q "\"authorized_user\": \"$(key_of AUTH)\"" "$FIXTURES/healthy.json" \
  || fail "the recorded healthy answer does not carry auth's key as an authorized_user"

expect healthy 0 \
  '^auth +own +cid 5$' '^camera +own +cid 6$' '^sync +own +cid 7$' \
  '3 expected service\(s\), 3 own, 0 missing, 0 not deployed; 3 connection\(s\), 0 SHARED, 0 unknown$' \
  '^nats-users-check: deployed services from --services$' '^nats-users-check: pass: '
[ "$(service_rows)" = "auth camera sync " ] || fail "healthy: the service table is $(service_rows)"

expect missing 1 \
  '^auth +own +cid 8$' '^camera +missing +no connection presented its key$' '^sync +own +cid 9$' \
  '^nats-users-check: fail; missing: camera; keep the shared user$'

expect unknown-key 1 \
  '^auth +own +cid 10$' '^camera +own +cid 11$' '^sync +own +cid 12$' \
  '^13 +unknown +172\.17\.0\.1 +U[A-Z2-7]+ is not in auth\.conf$' \
  '^nats-users-check: fail; unknown connection\(s\), cid 13; keep the shared user$'
[ "$(service_rows)" = "auth camera sync " ] || fail "unknown-key: a connection added a service: $(service_rows)"

expect shared 1 \
  '^auth +own +cid 14$' '^17 +SHARED +172\.17\.0\.1 +the shared user$' \
  '^nats-users-check: fail; SHARED connection\(s\), cid 17; keep the shared user$'

expect no-user 1 \
  '^5 +unknown +172\.17\.0\.1 +no authorized_user$' '^6 +unknown +172\.17\.0\.1 +no authorized_user$' \
  '^auth +missing ' '^camera +missing ' '^sync +missing ' \
  '^nats-users-check: fail; missing: auth camera sync; unknown connection\(s\), cid 5 6; keep the shared user$'

{
  cat "$FIXTURES/permissions.conf"
  printf '\nARGUS_NATS_GUARD: {\n  publish: {\n    allow: [\n      "argus.guard.v1.change"\n    ]\n  }\n}\n'
} > "$TEST_TMP/permissions-guard.conf"
gate "$FIXTURES/healthy.json" "$TEST_TMP/permissions-guard.conf" "$FIXTURES/auth.conf" "$THREE argus-guard"
[ "$status" = 1 ] || fail "a deployed permissions row with no connection did not fail the gate"
grep -Eq '^guard +missing +auth\.conf holds no key for it; re-run provision-host\.sh$' "$TEST_TMP/out" \
  || fail "a permissions row with no key in auth.conf was not named missing: $(cat "$TEST_TMP/out")"
gate "$FIXTURES/healthy.json" "$TEST_TMP/permissions-guard.conf" "$FIXTURES/auth.conf" "$THREE"
check_output "a row the stack does not deploy" 0 '^guard +not deployed +not in the stack'"'"'s active profiles$'

gate "$FIXTURES/profile-off.json" "$FIXTURES/permissions-tunnel.conf" "$FIXTURES/auth-tunnel.conf" "$THREE"
check_output "no tunnel profile" 0 \
  '^auth +own +cid 5$' '^camera +own +cid 6$' '^sync +own +cid 7$' \
  '^relay +not deployed +not in the stack'"'"'s active profiles$' \
  '^tunnel +not deployed +not in the stack'"'"'s active profiles$' \
  '3 expected service\(s\), 3 own, 0 missing, 2 not deployed; 3 connection\(s\), 0 SHARED, 0 unknown$' \
  '^nats-users-check: pass: '

gate "$FIXTURES/profile-on-relay-absent.json" "$FIXTURES/permissions-tunnel.conf" "$FIXTURES/auth-tunnel.conf" \
  "$THREE argus-relay argus-tunnel-client"
check_output "the tunnel profile without relay" 1 \
  '^relay +missing +no connection presented its key$' '^tunnel +own +cid 8$' \
  '5 expected service\(s\), 4 own, 1 missing, 0 not deployed; 4 connection\(s\), 0 SHARED, 0 unknown$' \
  '^nats-users-check: fail; missing: relay; keep the shared user$'

: > "$TEST_TMP/docker.log"
gate "$FIXTURES/healthy.json" "$FIXTURES/permissions.conf" "$FIXTURES/auth.conf" compose
check_output "the compose service list" 0 \
  '^nats-users-check: deployed services from docker compose config --services, profiles: ' \
  '^auth +own +cid 5$' '^nats-users-check: pass: '
[ "$(cat "$TEST_TMP/docker.log")" = "compose config --services" ] \
  || fail "the gate did not ask docker compose config --services: $(cat "$TEST_TMP/docker.log")"

set +e
COMPOSE_PROFILES=tunnel PATH="$TEST_TMP/bin:$PATH" ARGUS_GATE_CONNZ="$FIXTURES/profile-on-relay-absent.json" \
  ARGUS_GATE_DOCKER_LOG="$TEST_TMP/docker.log" "$ROOT/scripts/nats-users-check.sh" --monitor http://recorded \
  --auth-conf "$FIXTURES/auth-tunnel.conf" --permissions "$FIXTURES/permissions-tunnel.conf" > "$TEST_TMP/out" 2>&1
status=$?
set -e
check_output "the tunnel profile through compose" 1 \
  '^nats-users-check: deployed services from docker compose config --services, profiles: tunnel$' \
  '^relay +missing ' '^tunnel +own +cid 8$'

set +e
ARGUS_GATE_DOCKER_FAIL=1 gate "$FIXTURES/healthy.json" "$FIXTURES/permissions.conf" "$FIXTURES/auth.conf" compose
set -e
check_output "an unreadable compose config" 2 \
  '^nats-users-check: the compose config could not be read \(docker compose config --services in argus-deploy\): no configuration file provided: not found$'

gate "$FIXTURES/healthy.json" "$FIXTURES/permissions.conf" "$FIXTURES/auth.conf" "nats rustfs argus-tts"
check_output "a stack that deploys no row" 2 '^nats-users-check: the stack deploys no service of '

printf 'nats-users-check-test: 13 recorded cases ok\n'

if [ "$live" -eq 0 ]; then
  printf 'nats-users-check-test: the live broker check runs with --live\n'
  exit 0
fi

source "$ROOT/scripts/lib/nats-users.sh"
command -v docker > /dev/null 2>&1 || fail "--live needs docker"
image="$(sed -n 's/^[[:space:]]*image:[[:space:]]*\(nats:[^[:space:]]*\)$/\1/p' "$ROOT/argus-deploy/docker-compose.yml" | head -n 1)"
[ -n "$image" ] || fail "argus-deploy/docker-compose.yml names no nats image"
docker image inspect "$image" > /dev/null 2>&1 || fail "the image $image is not on this host"

port_free() {
  ! (exec 9<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

pick_port() {
  local candidate
  for _ in $(seq 1 50); do
    candidate=$(( 20000 + RANDOM % 40000 ))
    case "$candidate" in 4222|8222|9000|9001) continue ;; esac
    if port_free "$candidate"; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
}

client_port="$(pick_port)" || fail "no free loopback port"
monitor_port="$(pick_port)" || fail "no free loopback port"
[ "$client_port" != "$monitor_port" ] || fail "the two random ports collided"

mkdir -p "$TEST_TMP/live/seeds"
chmod 700 "$TEST_TMP/live/seeds"
printf 'ARGUS_NATS_AUTH: {\n  publish: {\n    allow: [\n      "argus.auth.v1.session"\n    ]\n  }\n}\n' \
  > "$TEST_TMP/live/permissions.conf"
nats_auth_conf "$TEST_TMP/live/permissions.conf" "$TEST_TMP/live/seeds" > "$TEST_TMP/live/auth.conf" \
  || fail "could not issue the live nkey"
public="$(nkey_public_of "$TEST_TMP/live/seeds/auth.seed")"
printf 'listen: "0.0.0.0:%s"\nhttp: "0.0.0.0:%s"\nauthorization {\n  users: [\n    {nkey: "%s"}\n  ]\n}\n' \
  "$client_port" "$monitor_port" "$public" > "$TEST_TMP/live/broker.conf"
chmod 644 "$TEST_TMP/live/broker.conf"

for _ in $(seq 1 10); do
  name="argus-gatecheck-nats-$(openssl rand -hex 4)"
  [ -z "$(docker ps -a --filter "name=^${name}\$" --format '{{.Names}}')" ] && break
  name=""
done
[ -n "$name" ] || fail "no unused container name"
container="$name"
docker run --rm -d --name "$container" \
  -p "127.0.0.1:$client_port:$client_port" -p "127.0.0.1:$monitor_port:$monitor_port" \
  -v "$TEST_TMP/live/broker.conf:/etc/gatecheck/nats.conf:ro" \
  "$image" -c /etc/gatecheck/nats.conf > /dev/null || fail "the throwaway broker did not start"
ready=0
for _ in $(seq 1 100); do
  if curl -fsS "http://127.0.0.1:$monitor_port/varz" > /dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
done
[ "$ready" -eq 1 ] || fail "the throwaway broker's monitor did not answer"

exec 3<>"/dev/tcp/127.0.0.1/$client_port"
IFS= read -r info <&3
nonce="$(sed -n 's/.*"nonce":"\([^"]*\)".*/\1/p' <<<"$info")"
[ -n "$nonce" ] || fail "the broker sent no nonce: $info"
printf '%s' "$nonce" > "$TEST_TMP/live/nonce"
signature="$(nkey_seed_payload < "$TEST_TMP/live/seeds/auth.seed" | nkey_private_pem \
  | openssl pkeyutl -sign -inkey /dev/stdin -rawin -in "$TEST_TMP/live/nonce" \
  | base64 -w0 | tr '+/' '-_' | tr -d '=')"
printf 'CONNECT {"verbose":false,"pedantic":false,"nkey":"%s","sig":"%s"}\r\nPING\r\n' \
  "$public" "$signature" >&3
IFS= read -r reply <&3
[ "${reply%$'\r'}" = "PONG" ] || fail "the nkey client was refused: $reply"

curl -fsS "http://127.0.0.1:$monitor_port/connz?auth=1&limit=4096" > "$TEST_TMP/live/connz.json" \
  || fail "the live monitor did not answer"
observed="$(grep -o '"authorized_user": "[^"]*"' "$TEST_TMP/live/connz.json" || true)"
[ "$observed" = "\"authorized_user\": \"$public\"" ] \
  || fail "authorized_user is not the nkey's public key: ${observed:-none} for $public"
printf 'nats-users-check-test: live: %s, the public key of the nkey the client presented\n' "$observed"

set +e
"$ROOT/scripts/nats-users-check.sh" --monitor "http://127.0.0.1:$monitor_port" \
  --auth-conf "$TEST_TMP/live/auth.conf" --permissions "$TEST_TMP/live/permissions.conf" \
  --services argus-auth > "$TEST_TMP/live/gate.out" 2>&1
status=$?
set -e
[ "$status" = 0 ] || fail "the gate did not pass the live broker: $(cat "$TEST_TMP/live/gate.out")"
grep -Eq '^auth +own +cid [0-9]+$' "$TEST_TMP/live/gate.out" \
  || fail "the gate did not count the live connection as auth's own: $(cat "$TEST_TMP/live/gate.out")"

exec 3>&-
docker rm -f "$container" > /dev/null 2>&1 || true
left="$(docker ps -a --filter "name=^${container}\$" --format '{{.Names}}')"
[ -z "$left" ] || fail "the throwaway broker $container still exists"
printf 'nats-users-check-test: live: the gate passed the live broker, and %s no longer exists\n' "$container"
container=""
