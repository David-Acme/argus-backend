#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: scripts/port-map-test.sh [--list]

Reads every listener port the stack declares and fails when two services
declare the same one. It reads the config templates (argus-deploy/config.*.toml.example
and services/*/config.toml.example): [server] port, grpc_port, health_port,
device_port and home_port, the app-facing [<service>] port, [remote]
tunnel_port, [rpc] address, [internal_tls] address and [streaming]
webrtc_listen; the compose publishes (host and container side) and listen
addresses; argus-deploy/nats.conf; argus-deploy/livekit.yaml.example; and
argus-deploy/livekit-tls.conf. A client target in another service's config is
not a listener and is not read. A port repeated within one service (its native
template, its deploy template, its compose publish) is one listener; services/tunnel
is the relay's native template. --list prints every port with its service and
where it was read.
USAGE
}

list=0
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --list) list=1 ;;
  "") ;;
  *) usage >&2; exit 2 ;;
esac

TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

fail() {
  printf 'port-map-test: %s\n' "$*" >&2
  exit 1
}

template_listeners() {
  local file="$1" owner="$2"
  awk -v owner="$owner" -v file="$file" '
    /^[[:space:]]*\[/ {
      section = $0
      gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", section)
      next
    }
    /^[[:space:]]*[A-Za-z0-9_]+[[:space:]]*=/ {
      key = $0
      sub(/^[[:space:]]*/, "", key)
      sub(/[[:space:]]*=.*$/, "", key)
      value = $0
      sub(/^[^=]*=[[:space:]]*/, "", value)
      sub(/[[:space:]]+#.*$/, "", value)
      gsub(/"/, "", value)
      port = ""
      if (section == "server" && key ~ /^(port|grpc_port|health_port|device_port|home_port)$/)
        port = value
      else if (section == owner && key == "port")
        port = value
      else if (section == "remote" && key == "tunnel_port")
        port = value
      else if ((section == "rpc" || section == "internal_tls") && key == "address")
        port = value
      else if (section == "streaming" && key == "webrtc_listen")
        port = value
      sub(/^.*:/, "", port)
      if (port ~ /^[0-9]+$/ && port + 0 > 0)
        printf "%d\t%s\t%s [%s] %s\n", port, owner, file, section, key
    }' "$1"
}

compose_listeners() {
  awk -v file="$1" '
    function owner_of(name) {
      sub(/^argus-/, "", name)
      if (name == "tunnel-client")
        name = "tunnel"
      return name
    }
    function emit(port, what) {
      if (port ~ /^[0-9]+$/ && port + 0 > 0)
        printf "%d\t%s\t%s %s %s\n", port, owner, file, service, what
    }
    /^services:/ { in_services = 1; next }
    in_services && /^[A-Za-z]/ { in_services = 0 }
    in_services && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {
      service = $1
      sub(/:$/, "", service)
      owner = owner_of(service)
      in_ports = 0
      next
    }
    in_services && /^    [A-Za-z_]+:/ {
      in_ports = ($1 == "ports:")
    }
    in_services && in_ports && /^      - / {
      entry = $0
      sub(/^      - /, "", entry)
      gsub(/"/, "", entry)
      sub(/\/(tcp|udp)$/, "", entry)
      container = entry
      sub(/^.*:/, "", container)
      emit(container, "ports (container)")
      host = entry
      sub(/:[0-9]+$/, "", host)
      if (match(host, /:-[0-9]+\}/)) {
        host = substr(host, RSTART + 2, RLENGTH - 3)
      }
      else {
        sub(/^.*:/, "", host)
      }
      emit(host, "ports (host)")
      next
    }
    in_services && /^      [A-Z_]*ADDRESS:[[:space:]]*"[^"]*:[0-9]+"/ {
      address = $0
      sub(/^[^:]*:[[:space:]]*/, "", address)
      gsub(/"/, "", address)
      sub(/^.*:/, "", address)
      emit(address, "environment address")
    }' "$1"
}

file_listeners() {
  local file="$1" owner="$2" pattern="$3"
  awk -v owner="$owner" -v file="$file" -v pattern="$pattern" '
    $0 ~ pattern {
      line = $0
      gsub(/"|;/, "", line)
      sub(/[[:space:]]+ssl$/, "", line)
      sub(/^.*[: ]/, "", line)
      if (line ~ /^[0-9]+$/)
        printf "%d\t%s\t%s line %d\n", line, owner, file, NR
    }' "$file"
}

inventory() {
  local root="$1" template name
  for template in "$root"/argus-deploy/config.*.toml.example; do
    name="${template##*/config.}"
    template_listeners "$template" "${name%.toml.example}"
  done
  for template in "$root"/services/*/config.toml.example; do
    name="${template%/config.toml.example}"
    name="${name##*/}"
    [ "$name" = tunnel ] && name=relay
    template_listeners "$template" "$name"
  done
  compose_listeners "$root/argus-deploy/docker-compose.yml"
  file_listeners "$root/argus-deploy/nats.conf" nats '^(port|http):'
  file_listeners "$root/argus-deploy/livekit.yaml.example" livekit '^[[:space:]]*(port|tcp_port|udp_port|tls_port):[[:space:]]*[0-9]+'
  file_listeners "$root/argus-deploy/livekit-tls.conf" livekit-tls '^[[:space:]]*listen[[:space:]]+[0-9]+'
}

duplicates() {
  awk -F '\t' '
    {
      key = $1 SUBSEP $2
      if (!(key in seen)) {
        seen[key] = 1
        owners[$1] = owners[$1] (owners[$1] == "" ? "" : ", ") $2 " (" $3 ")"
        count[$1]++
      }
    }
    END {
      for (port in count)
        if (count[port] > 1)
          printf "%s: %s\n", port, owners[port]
    }' "$1" | sort -n
}

inventory "$ROOT" | sed 's#'"$ROOT"'/##g' | sort -n -k1,1 -k2,2 > "$TEST_TMP/ports.tsv"
[ "$(wc -l < "$TEST_TMP/ports.tsv")" -ge 60 ] || fail "only $(wc -l < "$TEST_TMP/ports.tsv") listener declarations were read"
declared() {
  awk -F '\t' -v port="$1" -v owner="$2" '$1 == port && $2 == owner { found = 1 } END { exit !found }' "$TEST_TMP/ports.tsv"
}
for expected in "7042 auth" "7043 auth" "7129 tts" "7880 livekit" "7046 livekit-tls" "4222 nats" \
    "9000 rustfs" "7100 relay" "7104 tunnel" "8555 camera" "7035 voice"; do
  declared ${expected% *} ${expected#* } || fail "the inventory misses port ${expected% *} of ${expected#* }"
done
! awk -F '\t' '$3 ~ /\] ([a-z_]*target|[a-z_]*url|endpoint|relay_port|gateway_port)$/ { found = 1 } END { exit !found }' \
  "$TEST_TMP/ports.tsv" || fail "a client target was read as a listener"

if [ "$list" -eq 1 ]; then
  awk -F '\t' '{ printf "%-6s %-14s %s\n", $1, $2, $3 }' "$TEST_TMP/ports.tsv"
fi

duplicates "$TEST_TMP/ports.tsv" > "$TEST_TMP/duplicates"
if [ -s "$TEST_TMP/duplicates" ]; then
  fail "ports declared by more than one service: $(tr '\n' ';' < "$TEST_TMP/duplicates")"
fi

planted="$TEST_TMP/planted"
mkdir -p "$planted/argus-deploy" "$planted/services/tts"
cp "$ROOT"/argus-deploy/config.*.toml.example "$ROOT/argus-deploy/docker-compose.yml" \
  "$ROOT/argus-deploy/nats.conf" "$ROOT/argus-deploy/livekit.yaml.example" \
  "$ROOT/argus-deploy/livekit-tls.conf" "$planted/argus-deploy/"
sed 's/^port = 7029$/port = 7030/' "$ROOT/services/tts/config.toml.example" > "$planted/services/tts/config.toml.example"
inventory "$planted" > "$TEST_TMP/planted.tsv"
duplicates "$TEST_TMP/planted.tsv" > "$TEST_TMP/planted-duplicates"
grep -q '^7030: ' "$TEST_TMP/planted-duplicates" || fail "a port planted on two services was not reported"

owners="$(cut -f2 "$TEST_TMP/ports.tsv" | sort -u | wc -l)"
ports="$(cut -f1 "$TEST_TMP/ports.tsv" | sort -u | wc -l)"
printf 'port-map-test: %s listener ports across %s services, no port declared by two services\n' "$ports" "$owners"
