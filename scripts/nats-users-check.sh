#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONNZ_PATH='/connz?auth=1&limit=4096'

usage() {
  cat <<'USAGE'
usage: scripts/nats-users-check.sh [--monitor <http://host:port>] [--auth-conf <file>]

Lists every client connection of the broker with the user it authenticated
as: the service whose nkey it presented, or SHARED for the shared "argus"
user. Run it before the shared user is removed: that step waits until no
connection uses it.

Exit status: 0 when no connection uses the shared user, 1 when one does,
2 when the broker or auth.conf could not be read or no client is connected.

By default the broker is asked from inside the stack's nats container
(docker compose exec; the monitor listens on the container's loopback 8222)
and the service names come from ${ARGUS_DATA_DIR}/nats/auth.conf, with
ARGUS_DATA_DIR read from argus-deploy/.env. --monitor asks a monitor base URL
directly; --auth-conf names another auth.conf. Nothing is changed.
USAGE
}

monitor=""
auth_conf=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --monitor) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; monitor="$2"; shift 2 ;;
    --auth-conf) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; auth_conf="$2"; shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done

if [ -z "$auth_conf" ]; then
  data_dir="$(sed -n 's/^ARGUS_DATA_DIR=//p' "$ROOT/argus-deploy/.env" 2>/dev/null | tail -n 1)"
  auth_conf="${data_dir:-$ROOT/argus-deploy/data}/nats/auth.conf"
fi
if [ ! -r "$auth_conf" ]; then
  printf 'nats-users-check: cannot read %s\n' "$auth_conf" >&2
  exit 2
fi

fetch_connz() {
  if [ -n "$monitor" ]; then
    curl -fsS "${monitor%/}$CONNZ_PATH"
  else
    (cd "$ROOT/argus-deploy" && docker compose exec -T nats wget -qO- "http://127.0.0.1:8222$CONNZ_PATH")
  fi
}

if ! connz="$(fetch_connz)"; then
  printf 'nats-users-check: the broker monitor did not answer\n' >&2
  exit 2
fi

key_services="$(sed -n 's/^ *{nkey: "\(U[A-Z2-7]*\)", permissions: \$ARGUS_NATS_\([A-Z0-9_]*\)}$/\1 \2/p' "$auth_conf" \
  | awk '{ print $1 " " tolower($2) }' | tr '_' '-')"

connections="$(printf '%s\n' "$connz" | awk '
  function value(line) {
    sub(/^[^:]*:[[:space:]]*/, "", line)
    sub(/,[[:space:]]*$/, "", line)
    gsub(/"/, "", line)
    return line
  }
  function flush() {
    if (cid != "")
      print cid "\t" (user == "" ? "-" : user) "\t" (ip == "" ? "-" : ip) "\t" (name == "" ? "-" : name)
    cid = ""; user = ""; ip = ""; name = ""
  }
  /"connections":[[:space:]]*\[/ { inside = 1; next }
  !inside { next }
  /"cid":/ { flush(); cid = value($0); next }
  /"authorized_user":/ { user = value($0); next }
  /"ip":/ { ip = value($0); next }
  /"name":/ { name = value($0); next }
  END { flush() }
')"

shared=0
total=0
seen=""
printf '%-6s %-14s %-16s %s\n' "cid" "user" "ip" "name"
while IFS=$'\t' read -r cid user ip name; do
  [ -n "$cid" ] || continue
  total=$(( total + 1 ))
  if [ "$user" = "argus" ]; then
    label="SHARED"
    shared=$(( shared + 1 ))
  else
    label="$(awk -v key="$user" '$1 == key { print $2 }' <<<"$key_services")"
    [ -n "$label" ] || label="unknown:$user"
    seen="$seen $label"
  fi
  printf '%-6s %-14s %-16s %s\n' "$cid" "$label" "$ip" "$name"
done <<<"$connections"

missing=""
while read -r _ service; do
  [ -n "$service" ] || continue
  case " $seen " in
    *" $service "*) ;;
    *) missing="$missing $service" ;;
  esac
done <<<"$key_services"

printf 'nats-users-check: %d connection(s), %d on the shared user\n' "$total" "$shared"
if [ "$total" -eq 0 ]; then
  printf 'nats-users-check: no client is connected; bring the stack up and run it again\n'
  exit 2
fi
[ -z "$missing" ] || printf 'nats-users-check: services with no connection of their own:%s\n' "$missing"
if [ "$shared" -gt 0 ]; then
  printf 'nats-users-check: the shared user is still in use; keep it\n'
  exit 1
fi
printf 'nats-users-check: no connection uses the shared user\n'
