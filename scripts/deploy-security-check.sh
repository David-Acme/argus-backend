#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"
source "$ROOT/scripts/lib/nats-users.sh"

usage() {
  cat <<'USAGE'
usage: scripts/deploy-security-check.sh [options]

Checks a DEPLOYED Argus stack for the four internal-security properties of the
deploy: every service boots with [internal_tls] and no plaintext internal
listener, every internal caller dials a tls: target, every NATS client
connects with its own nkey and none with the shared "argus" user, and every
object-store reader uses its own bucket-scoped credentials instead of the
shared key pair. One line per check per service, starting green or red; a line
starting with "expected" names a gap only a provisioning pass on the deployed
configs can close (the check counts it apart from the reds and never changes
anything). The exit status is the number of red lines. It changes nothing: it reads the
deploy configs and the host state, opens read-only probes (a mutual TLS
handshake, a bucket listing), and asks the broker's monitor what is connected.

Options:
  --deploy-dir DIR     deploy directory holding config.<service>.toml and the
                       compose file (default: $ARGUS_DEPLOY_DIR, else
                       <repo>/argus-deploy)
  --data-dir DIR       host state: nats seeds, auth.conf, internal certs and
                       rustfs accounts (default: $ARGUS_DATA_DIR, else the
                       deploy .env's ARGUS_DATA_DIR, else <deploy-dir>/data)
  --compose-file FILE  compose file (default: <deploy-dir>/docker-compose.yml)
  --project NAME       compose project name, for the published ports and the
                       broker monitor (default: $COMPOSE_PROJECT_NAME, else the
                       compose file's name:, else argus-cutover)
  --services "a b"     only these services (default: every deploy config)
  --template-dir DIR   the templates the deployed configs came from, the
                       config.<service>.toml.example files whose listeners and
                       credentials the check compares a deployed config
                       against (default: <repo>/argus-deploy)
  --monitor URL        broker monitor base URL (default: the nats container's
                       own http://127.0.0.1:8222 through docker compose exec)
  --rustfs-endpoint URL  object store to probe (default: the storage.s3
                       endpoint of each config, rustfs mapped to the host)
  --timeout SECONDS    probe timeout (default: 5)
  --no-docker          resolve no published port and ask no container; every
                       probe uses the address the configs name
  --no-probe           the static checks alone: no handshake, no listing, no
                       monitor

Environment: ARGUS_DEPLOY_DIR, ARGUS_DATA_DIR, ARGUS_CERTS_DIR (only to
resolve a native certificate directory), COMPOSE_PROJECT_NAME, DOCKER.
USAGE
}

DEPLOY_DIR="${ARGUS_DEPLOY_DIR:-$ROOT/argus-deploy}"
TEMPLATE_DIR="${ARGUS_TEMPLATE_DIR:-$ROOT/argus-deploy}"
DATA_DIR="${ARGUS_DATA_DIR:-}"
COMPOSE_FILE=""
PROJECT="${COMPOSE_PROJECT_NAME:-}"
SERVICES_GIVEN=""
MONITOR=""
RUSTFS_ENDPOINT=""
TIMEOUT=5
USE_DOCKER=1
PROBE=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --deploy-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; DEPLOY_DIR="$2"; shift 2 ;;
    --data-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; DATA_DIR="$2"; shift 2 ;;
    --compose-file) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; COMPOSE_FILE="$2"; shift 2 ;;
    --project) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; PROJECT="$2"; shift 2 ;;
    --services) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; SERVICES_GIVEN="$2"; shift 2 ;;
    --template-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; TEMPLATE_DIR="$2"; shift 2 ;;
    --monitor) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; MONITOR="$2"; shift 2 ;;
    --rustfs-endpoint) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; RUSTFS_ENDPOINT="$2"; shift 2 ;;
    --timeout) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; TIMEOUT="$2"; shift 2 ;;
    --no-docker) USE_DOCKER=0; shift ;;
    --no-probe) PROBE=0; shift ;;
    *) usage >&2; exit 2 ;;
  esac
done

DEPLOY_DIR="$(realpath -m "$DEPLOY_DIR")"
[ -d "$DEPLOY_DIR" ] || { printf 'deploy-security-check: no deploy directory %s\n' "$DEPLOY_DIR" >&2; exit 2; }
[ -z "$COMPOSE_FILE" ] && COMPOSE_FILE="$DEPLOY_DIR/docker-compose.yml"

env_value() {
  local file="$1" key="$2"
  [ -r "$file" ] || return 0
  sed -n "s/^$key=//p" "$file" 2>/dev/null | tail -n 1 || true
}

if [ -z "$DATA_DIR" ]; then
  DATA_DIR="$(env_value "$DEPLOY_DIR/.env" ARGUS_DATA_DIR)"
fi
[ -n "$DATA_DIR" ] || DATA_DIR="$DEPLOY_DIR/data"
DATA_DIR="$(realpath -m "$DATA_DIR")"

if [ -z "$PROJECT" ]; then
  PROJECT="$(sed -n 's/^name:[[:space:]]*//p' "$COMPOSE_FILE" 2>/dev/null | head -n 1 || true)"
fi
[ -n "$PROJECT" ] || PROJECT="argus-cutover"

if [ "$USE_DOCKER" -eq 1 ]; then
  command -v docker >/dev/null 2>&1 || USE_DOCKER=0
  [ -f "$COMPOSE_FILE" ] || USE_DOCKER=0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

GREEN=0
RED=0
EXPECTED=0

report() {
  local service="$1" leg="$2" state="$3" detail="$4"
  case "$state" in
    green) GREEN=$(( GREEN + 1 )) ;;
    expected) EXPECTED=$(( EXPECTED + 1 )) ;;
    *) RED=$(( RED + 1 )) ;;
  esac
  printf '%-13s %-12s %-8s %s\n' "$service" "$leg" "$state" "$detail"
}

config_of() {
  printf '%s/config.%s.toml' "$DEPLOY_DIR" "$1"
}

compose_port() {
  local container="$1" port="$2" value
  [ "$USE_DOCKER" -eq 1 ] || return 1
  value="$(docker compose -f "$COMPOSE_FILE" -p "$PROJECT" port "$container" "$port" 2>/dev/null | head -n 1)" || return 1
  case "$value" in
    *:[0-9]*) printf '%s' "${value##*:}" ;;
    *) return 1 ;;
  esac
}

host_port() {
  local container="$1" port="$2"
  compose_port "$container" "$port" || printf '%s' "$port"
}

port_of() {
  case "$1" in
    *:[0-9]*) printf '%s' "${1##*:}" ;;
    *) printf '' ;;
  esac
}

tcp_open() {
  local host="$1" port="$2"
  timeout "$TIMEOUT" bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null
}

tls_handshake() {
  local host="$1" port="$2" cert="$3" key="$4" ca="$5" output
  output="$(timeout "$TIMEOUT" openssl s_client -connect "$host:$port" -cert "$cert" -key "$key" \
    -CAfile "$ca" -verify_return_error -brief </dev/null 2>&1)"
  case "$output" in
    *"Verification: OK"*) return 0 ;;
  esac
  printf '%s' "$output" | tail -n 1 > "$WORK/tls-last"
  return 1
}

tls_refuses_without_certificate() {
  local host="$1" port="$2" ca="$3"
  ! timeout "$TIMEOUT" openssl s_client -connect "$host:$port" -CAfile "$ca" \
    -tls1_2 -verify_return_error -brief </dev/null 2>&1 | grep -q 'Verification: OK'
}

internal_dir_of() {
  local service="$1" dir
  dir="$(toml_value "$(config_of "$service")" internal_tls dir)"
  if [ -n "$dir" ] && [ -s "$dir/cert.pem" ]; then
    printf '%s' "$dir"
    return 0
  fi
  if [ -s "$DATA_DIR/certs/internal/$service/cert.pem" ]; then
    printf '%s' "$DATA_DIR/certs/internal/$service"
    return 0
  fi
  printf '%s' "$dir"
}

certificate_pair_matches() {
  local cert="$1" key="$2"
  openssl x509 -in "$cert" -noout -pubkey > "$WORK/cert-pub" 2>/dev/null &&
    openssl pkey -in "$key" -pubout > "$WORK/key-pub" 2>/dev/null &&
    [ -s "$WORK/cert-pub" ] && cmp -s "$WORK/cert-pub" "$WORK/key-pub"
}

certificate_problem() {
  local service="$1" dir="$2" cert="$dir/cert.pem" key="$dir/key.pem" ca="$dir/ca.pem"
  local subject

  [ -n "$dir" ] || { printf '[internal_tls] dir is empty'; return 0; }
  [ -s "$cert" ] || { printf '%s/cert.pem is missing or empty' "$dir"; return 0; }
  [ -s "$key" ] || { printf '%s/key.pem is missing or empty' "$dir"; return 0; }
  [ -s "$ca" ] || { printf '%s/ca.pem is missing or empty' "$dir"; return 0; }
  if ! openssl x509 -in "$cert" -noout >/dev/null 2>&1; then
    printf '%s/cert.pem does not parse' "$dir"; return 0
  fi
  if ! openssl pkey -in "$key" -noout >/dev/null 2>&1; then
    printf '%s/key.pem does not parse' "$dir"; return 0
  fi
  if ! openssl x509 -in "$cert" -noout -checkend 0 >/dev/null 2>&1; then
    printf '%s/cert.pem is expired' "$dir"; return 0
  fi
  if ! openssl verify -CAfile "$ca" "$cert" >/dev/null 2>&1; then
    printf '%s/cert.pem is not issued by %s/ca.pem' "$dir" "$dir"; return 0
  fi
  if ! certificate_pair_matches "$cert" "$key"; then
    printf '%s/key.pem does not match %s/cert.pem' "$dir" "$dir"; return 0
  fi
  subject="$(openssl x509 -in "$cert" -noout -subject -nameopt RFC2253 2>/dev/null | sed 's/^subject=[[:space:]]*//')"
  case "$subject" in
    "CN=argus-$service,"*) ;;
    *) printf '%s/cert.pem names %s, not argus-%s' "$dir" "$subject" "$service"; return 0 ;;
  esac
  return 1
}

plain_listener_of() {
  local config="$1" value
  value="$(toml_value "$config" server grpc_port)"
  if [ -n "$value" ]; then
    printf 'server.grpc_port = %s' "$value"
    return 0
  fi
  value="$(toml_value "$config" rpc address)"
  if [ -n "$value" ]; then
    printf 'rpc.address = %s' "$value"
    return 0
  fi
}

plain_targets_of() {
  local config="$1"
  awk '
    {
      line = $0
      sub(/^[[:space:]]*/, "", line)
      if (line ~ /^#/)
        next
      key = line
      sub(/^\[.*\]/, "", key)
      if (key !~ /target[[:space:]]*=/)
        next
      name = key
      sub(/[[:space:]]*=.*$/, "", name)
      if (name !~ /target$/)
        next
      value = key
      sub(/^[^=]*=[[:space:]]*/, "", value)
      sub(/[[:space:]]*$/, "", value)
      if (value ~ /^".*"$/)
        value = substr(value, 2, length(value) - 2)
      if (value == "" || value ~ /^tls:/)
        next
      section = ""
      if (match(line, /^\[[^]]*\]/))
        section = substr(line, RSTART, RLENGTH)
      print name "\t" value "\t" section
    }' "$config"
}

leg_expected_internal_tls() {
  local service="$1" config="$2"
  local template="$TEMPLATE_DIR/config.$service.toml.example" address

  [ -f "$template" ] || return 0
  address="$(toml_value "$template" internal_tls address)"
  [ -n "$address" ] || return 0
  report "$service" internal-tls expected "the deployed config has no [internal_tls], while $template declares $address: enable [internal_tls] in $(config_of "$service") and pair its caller credentials by re-running scripts/provision-host.sh, then restart argus-$service"
}

leg_internal_tls() {
  local service="$1" config="$2" dir address host port container detail plain template

  dir="$(internal_dir_of "$service")"
  if detail="$(certificate_problem "$service" "$dir")"; then
    report "$service" internal-tls red "$detail"
    return
  fi
  plain="$(plain_listener_of "$config")"
  if [ -n "$plain" ]; then
    template="$TEMPLATE_DIR/config.$service.toml.example"
    if [ -f "$template" ] && [ -n "$(plain_listener_of "$template")" ]; then
      report "$service" internal-tls red "$dir is valid, but the config declares the plaintext listener $plain, and so does $template"
    else
      report "$service" internal-tls expected "$dir is valid and the config also declares the plaintext listener $plain, which its template no longer does: re-run scripts/provision-host.sh, which retires it from $(config_of "$service"), then restart argus-$service"
    fi
    return
  fi
  address="$(toml_value "$config" internal_tls address)"
  if [ -z "$address" ]; then
    report "$service" internal-tls red "$dir is valid, but [internal_tls] address is empty"
    return
  fi
  if [ "$PROBE" -eq 0 ]; then
    report "$service" internal-tls green "certificate of $service in $dir, no plaintext listener, [internal_tls] address = $address"
    return
  fi
  port="$(port_of "$address")"
  if [ -z "$port" ]; then
    report "$service" internal-tls red "[internal_tls] address $address names no port"
    return
  fi
  container="$(host_port "argus-$service" "$port")"
  host=127.0.0.1
  if ! tcp_open "$host" "$container"; then
    report "$service" internal-tls red "nothing answers on $host:$container ([internal_tls] address = $address, host port: $container)"
    return
  fi
  if ! tls_handshake "$host" "$container" "$dir/cert.pem" "$dir/key.pem" "$dir/ca.pem"; then
    report "$service" internal-tls red "the mutual TLS handshake on $host:$container failed: $(cat "$WORK/tls-last" 2>/dev/null)"
    return
  fi
  if ! tls_refuses_without_certificate "$host" "$container" "$dir/ca.pem"; then
    report "$service" internal-tls red "$host:$container answers a client without a certificate"
    return
  fi
  report "$service" internal-tls green "certificate of $service in $dir, mutual TLS on $host:$container, a client without a certificate refused"
}

caller_target_service() {
  local value="$1" host part

  host="$value"
  case "$value" in
    *://*) host="${value#*://}" ;;
    tls:*) host="${value#tls:}" ;;
  esac
  part="${host%%:*}"
  printf '%s' "${part#argus-}"
}

target_service_template_has_tls() {
  local service="$1"
  local template="$TEMPLATE_DIR/config.$service.toml.example"

  [ -f "$template" ] || return 1
  [ -n "$(toml_value "$template" internal_tls address)" ]
}

leg_callers() {
  local service="$1" config="$2" offenders count template placeholders caller
  local name value section target moves=""

  offenders="$(plain_targets_of "$config")"
  count="$(grep -c 'target[[:space:]]*=' "$config" 2>/dev/null || true)"
  if [ -n "$offenders" ]; then
    while IFS=$'\t' read -r name value section; do
      [ -n "$name" ] || continue
      target="$(caller_target_service "$value")"
      if target_service_template_has_tls "$target"; then
        moves="$moves $name=$value"
        continue
      fi
      report "$service" callers red "not on tls: $name = $value"
      return
    done <<<"$offenders"
    report "$service" callers expected "the deployed config dials$moves on plaintext, while the templates declare the mutual TLS listener of the target: set each target to its tls: form and pair the caller credential by re-running scripts/provision-host.sh, then restart the caller(s)"
    return
  fi
  if [ "${count:-0}" -eq 0 ]; then
    return
  fi
  template="$TEMPLATE_DIR/config.$service.toml.example"
  [ -f "$template" ] || { report "$service" callers green "$count internal dial target(s), all on tls:"; return; }
  if [ -f "$template" ]; then
    placeholders=""
    for caller in $(awk '/^\[rpc\.callers\]$/ { in_table = 1; next } /^\[/ { in_table = 0 } in_table && /=/ { sub(/[[:space:]]*=.*$/, ""); print }' "$template"); do
      case "$(toml_value "$config" rpc.callers "$caller")" in
        "" | *CHANGE_ME*) placeholders="$placeholders $caller" ;;
      esac
    done
    if [ -n "$placeholders" ]; then
      report "$service" callers expected "the deployed config pairs no caller credential for$placeholders; re-run scripts/provision-host.sh, which brings the paired credentials of $template into $(config_of "$service"), then restart argus-$service"
      return
    fi
  fi
  report "$service" callers green "$count internal dial target(s), all on tls:"
}

leg_nats() {
  local service="$1" config="$2" nkey seed seed_path url public auth_conf

  nkey="$(toml_value "$config" nats nkey)"
  if [ -z "$nkey" ]; then
    report "$service" nats red "[nats] nkey is empty; run scripts/provision-host.sh"
    return
  fi
  if [ -n "$(toml_value "$config" nats user)" ] || [ -n "$(toml_value "$config" nats password)" ]; then
    report "$service" nats red "[nats] still names the shared argus user (user/password); its clients can fall back to it"
    return
  fi
  seed="$(toml_value "$config" nats seed_file)"
  if [ -z "$seed" ]; then
    report "$service" nats red "[nats] seed_file is empty, so the key $nkey cannot be presented"
    return
  fi
  seed_path="$DATA_DIR/nats/seeds/$service.seed"
  if [ ! -s "$seed_path" ]; then
    report "$service" nats red "no seed at $seed_path for the configured key $nkey"
    return
  fi
  if ! public="$(nkey_public_of "$seed_path")"; then
    report "$service" nats red "the seed $seed_path does not decode"
    return
  fi
  if [ "$public" != "$nkey" ]; then
    report "$service" nats red "[nats] nkey = $nkey, but $seed_path holds the key $public"
    return
  fi
  auth_conf="$DATA_DIR/nats/auth.conf"
  if [ ! -r "$auth_conf" ]; then
    report "$service" nats red "the broker's auth.conf $auth_conf is unreadable"
    return
  fi
  if ! grep -Fq "\"$nkey\"" "$auth_conf"; then
    report "$service" nats red "auth.conf holds no entry for the key $nkey; re-run scripts/provision-host.sh"
    return
  fi
  if grep -Eq 'user:[[:space:]]*"argus"' "$auth_conf"; then
    report "$service" nats red "auth.conf still emits the shared argus user"
    return
  fi
  if [ "$PROBE" -eq 0 ]; then
    report "$service" nats green "own key $nkey, its seed in $seed_path, admitted by auth.conf alone"
    return
  fi
  url="$(toml_value "$config" nats url)"
  if [ "$monitor_tried" -eq 0 ]; then
    monitor_tried=1
    if [ -n "$MONITOR" ]; then
      monitor_answer="$(curl -fsS "${MONITOR%/}/connz?auth=1&limit=4096" 2>/dev/null)" || monitor_answer=""
    elif [ "$USE_DOCKER" -eq 1 ]; then
      monitor_answer="$(docker compose -f "$COMPOSE_FILE" -p "$PROJECT" exec -T nats wget -qO- "http://127.0.0.1:8222/connz?auth=1&limit=4096" 2>/dev/null)" || monitor_answer=""
    fi
  fi
  if [ -z "$monitor_answer" ] || ! grep -Eq '"connections":[[:space:]]*\[' <<<"$monitor_answer"; then
    report "$service" nats red "the broker monitor of $url did not answer, so its own key $nkey was not seen connected"
    return
  fi
  known_nkeys="$known_nkeys $nkey"
  if grep -Eq "\"authorized_user\":[[:space:]]*\"$nkey\"" <<<"$monitor_answer"; then
    report "$service" nats green "connected to $url as its own key $nkey, auth.conf holds no shared argus user"
    return
  fi
  report "$service" nats red "no connection to $url presents its own key $nkey"
}

leg_storage() {
  local service="$1" config="$2" endpoint credentials host port prefix outside code body

  endpoint="$(toml_value "$config" storage.s3 endpoint)"
  credentials="$(toml_value "$config" storage.s3 credentials_file)"
  if [ -n "$(toml_value "$config" storage.s3 access_key)" ] || [ -n "$(toml_value "$config" storage.s3 secret_key)" ]; then
    report "$service" storage red "storage.s3 still carries the shared access_key/secret_key"
    return
  fi
  if [ -z "$credentials" ]; then
    report "$service" storage red "storage.s3.credentials_file is empty; the reader has no bucket-scoped pair"
    return
  fi
  if [ ! -s "$DATA_DIR/rustfs/accounts/$service/credentials.json" ]; then
    report "$service" storage red "no account pair at $DATA_DIR/rustfs/accounts/$service/credentials.json"
    return
  fi
  if ! grep -q '"accessKey"' "$DATA_DIR/rustfs/accounts/$service/credentials.json" ||
     ! grep -q '"secretKey"' "$DATA_DIR/rustfs/accounts/$service/credentials.json"; then
    report "$service" storage red "$DATA_DIR/rustfs/accounts/$service/credentials.json holds no pair"
    return
  fi
  if [ "$PROBE" -eq 0 ]; then
    report "$service" storage green "bucket-scoped pair in $DATA_DIR/rustfs/accounts/$service/credentials.json, no shared key in the config"
    return
  fi
  case "$service" in
    identity) prefix="portraits/"; outside="cameras/" ;;
    camera) prefix="cameras/"; outside="portraits/" ;;
    guard) prefix="guard/incidents/"; outside="portraits/" ;;
    *) prefix=""; outside="" ;;
  esac
  if [ -z "$prefix" ]; then
    report "$service" storage red "this check knows no object prefix of $service"
    return
  fi
  host=127.0.0.1
  if [ -n "$RUSTFS_ENDPOINT" ]; then
    endpoint="$RUSTFS_ENDPOINT"
  else
    port="$(port_of "$endpoint")"
    port="$(host_port rustfs "$port")"
    endpoint="${endpoint%%://*}://$host:$port"
  fi
  body="$WORK/curl-$service.conf"
  {
    printf 'user = "'
    awk -F'"' '{ for (i = 1; i < NF; i++) if ($i == "accessKey" || $i == "secretKey") printf "%s%s", (seen++ ? ":" : ""), $(i + 2) }' \
      "$DATA_DIR/rustfs/accounts/$service/credentials.json"
    printf '"\n'
  } > "$body"
  code="$(curl -q -sS -K "$body" --aws-sigv4 "aws:amz:us-east-1:s3" -o "$WORK/list-$service.out" -w '%{http_code}' \
    --max-time "$TIMEOUT" "$endpoint/$(toml_value "$config" storage.s3 bucket)?list-type=2&max-keys=0&prefix=$prefix" 2>/dev/null)" || code=""
  if [ "$code" != 200 ]; then
    report "$service" storage red "its own pair does not list $prefix in $endpoint (HTTP ${code:-none})"
    return
  fi
  code="$(curl -q -sS -K "$body" --aws-sigv4 "aws:amz:us-east-1:s3" -o "$WORK/list-outside-$service.out" -w '%{http_code}' \
    --max-time "$TIMEOUT" "$endpoint/$(toml_value "$config" storage.s3 bucket)?list-type=2&max-keys=0&prefix=$outside" 2>/dev/null)" || code=""
  case "$code" in
    403)
      report "$service" storage green "its own bucket-scoped pair lists $prefix and is refused $outside in $endpoint"
      ;;
    *)
      report "$service" storage red "its own pair lists $outside in $endpoint (HTTP ${code:-none}), so it is not scoped to $prefix"
      ;;
  esac
}

printf 'deploy-security-check: %s, data %s, project %s%s\n' \
  "$DEPLOY_DIR" "$DATA_DIR" "$PROJECT" "$([ "$PROBE" -eq 1 ] && printf '' || printf ', no probe')"

monitor_answer=""
monitor_tried=0
known_nkeys=""

services=""
for config in "$DEPLOY_DIR"/config.*.toml; do
  [ -f "$config" ] || continue
  case "${config##*/}" in
    *.example) continue ;;
  esac
  name="${config##*/config.}"
  name="${name%.toml}"
  services="$services $name"
done

if [ -n "$SERVICES_GIVEN" ]; then
  filtered=""
  for name in $(tr ', ' '\n\n' <<<"$SERVICES_GIVEN"); do
    [ -f "$(config_of "$name")" ] || { printf 'deploy-security-check: no %s\n' "$(config_of "$name")" >&2; exit 2; }
    filtered="$filtered $name"
  done
  services="$filtered"
fi

if [ -z "${services// /}" ]; then
  printf 'deploy-security-check: no config.<service>.toml under %s\n' "$DEPLOY_DIR" >&2
  exit 2
fi

for service in $services; do
  config="$(config_of "$service")"
  if toml_key_exists "$config" internal_tls address; then
    leg_internal_tls "$service" "$config"
  else
    leg_expected_internal_tls "$service" "$config"
  fi
  leg_callers "$service" "$config"
  if toml_key_exists "$config" nats url || toml_key_exists "$config" nats nkey; then
    leg_nats "$service" "$config"
  fi
  if [ "$(toml_value "$config" storage mode)" = "s3" ]; then
    leg_storage "$service" "$config"
  fi
done

if [ "$PROBE" -eq 1 ] && [ -n "$monitor_answer" ]; then
  connected_keys="$(grep -Eo '"authorized_user":[[:space:]]*"[^"]*"' <<<"$monitor_answer" | sed 's/.*"\([^"]*\)"$/\1/' || true)"
  while read -r key; do
    [ -n "$key" ] || continue
    if [ "$key" = argus ]; then
      report broker nats red "a client is connected as the shared argus user"
      continue
    fi
    case " $known_nkeys " in
      *" $key "*) ;;
      *) report broker nats red "a client is connected as $key, which is no configured service's key" ;;
    esac
  done <<<"$connected_keys"
fi

printf 'deploy-security-check: %s green, %s expected, %s red\n' "$GREEN" "$EXPECTED" "$RED"
exit "$RED"
