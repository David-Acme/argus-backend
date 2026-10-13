#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONNZ_PATH='/connz?auth=1&limit=4096'

source "$ROOT/scripts/lib/nats-users.sh"

usage() {
  cat <<'USAGE'
usage: scripts/nats-users-check.sh [--monitor <http://host:port>] [--auth-conf <file>]
                                   [--permissions <file>] [--services <list>]

The gate before the shared NATS user is removed. The expected services are the
rows of argus-deploy/nats-permissions.conf, the list provisioning issues one
nkey per service from, that the stack deploys with its active profiles: the
services docker compose config --services prints in argus-deploy, where
compose reads the profiles from COMPOSE_PROFILES or from .env exactly as when
it starts the stack. A compose service argus-<name> or argus-<name>-client
deploys the row <name>. A row the stack does not deploy is listed as not
deployed and does not fail the gate. What happens to be connected never adds
or removes a service.

Every broker connection is matched by its authorized_user, which the broker
reports as the nkey's public key for an nkey user and as the user name for a
password user:

  own       the key auth.conf records for a row of the permissions
  SHARED    the shared "argus" user
  unknown   a key auth.conf does not record, a key of a service the
            permissions do not list, or no authorized_user at all

Each expected service is then own (at least one connection of its own) or
missing (none). The output lists every service with its state, every
connection with its state, and the reason for the exit.

Exit status: 0 when every expected service has a connection of its own and no
connection is SHARED or unknown; 1 when one is missing, SHARED or unknown; 2
when the broker, auth.conf, the permissions or the compose config could not be
read, or when the stack deploys no row of the permissions.

By default the broker is asked from inside the stack's nats container
(docker compose exec; the monitor listens on the container's loopback 8222),
auth.conf is ${ARGUS_DATA_DIR}/nats/auth.conf with ARGUS_DATA_DIR read from
argus-deploy/.env, and the permissions are argus-deploy/nats-permissions.conf.
--monitor asks a monitor base URL directly; --auth-conf and --permissions
name other files; --services gives the compose service names, separated by
spaces or commas, instead of asking docker compose. Nothing is changed.
USAGE
}

monitor=""
auth_conf=""
permissions="$ROOT/argus-deploy/nats-permissions.conf"
services_given=0
services_list=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --monitor) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; monitor="$2"; shift 2 ;;
    --auth-conf) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; auth_conf="$2"; shift 2 ;;
    --permissions) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; permissions="$2"; shift 2 ;;
    --services) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; services_given=1; services_list="$2"; shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

env_value() {
  [ -r "$ROOT/argus-deploy/.env" ] || return 0
  sed -n "s/^$1=//p" "$ROOT/argus-deploy/.env" | tail -n 1
}

if [ -z "$auth_conf" ]; then
  data_dir="$(env_value ARGUS_DATA_DIR)"
  auth_conf="${data_dir:-$ROOT/argus-deploy/data}/nats/auth.conf"
fi
if [ ! -r "$auth_conf" ]; then
  printf 'nats-users-check: cannot read %s\n' "$auth_conf" >&2
  exit 2
fi
if [ ! -r "$permissions" ]; then
  printf 'nats-users-check: cannot read %s\n' "$permissions" >&2
  exit 2
fi

rows="$(nats_permission_services "$permissions")"
if [ -z "$rows" ]; then
  printf 'nats-users-check: %s lists no service\n' "$permissions" >&2
  exit 2
fi

if [ "$services_given" -eq 1 ]; then
  compose_services="$(tr ', ' '\n\n' <<<"$services_list")"
  source_note="--services"
else
  if ! compose_services="$(cd "$ROOT/argus-deploy" && docker compose config --services 2> "$work/compose.err")"; then
    printf 'nats-users-check: the compose config could not be read (docker compose config --services in argus-deploy): %s\n' \
      "$(tail -n 1 "$work/compose.err")" >&2
    exit 2
  fi
  profiles="${COMPOSE_PROFILES-}"
  if [ -z "${COMPOSE_PROFILES+set}" ]; then
    profiles="$(env_value COMPOSE_PROFILES)"
  fi
  source_note="docker compose config --services, profiles: ${profiles:-none}"
fi
deployed="$(sed -n 's/^argus-//p' <<<"$compose_services" | sed 's/-client$//' | sort -u)"
expected="$(grep -Fxf <(printf '%s\n' "$deployed") <<<"$rows" || true)"
if [ -z "$expected" ]; then
  printf 'nats-users-check: the stack deploys no service of %s (%s)\n' "$permissions" "$source_note" >&2
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
if ! grep -Eq '"connections":[[:space:]]*\[' <<<"$connz"; then
  printf 'nats-users-check: the broker monitor answered without a connection list\n' >&2
  exit 2
fi

field_number() {
  { grep -Eo "\"$1\":[[:space:]]*[0-9]+" <<<"$connz" || true; } | head -n 1 | grep -Eo '[0-9]+$' || true
}
listed="$(field_number num_connections)"
total="$(field_number total)"
if [ -n "$listed" ] && [ -n "$total" ] && [ "$listed" -lt "$total" ]; then
  printf 'nats-users-check: the monitor listed %s of %s connections\n' "$listed" "$total" >&2
  exit 2
fi

key_services="$(sed -n 's/^ *{nkey: "\(U[A-Z2-7]*\)", permissions: \$ARGUS_NATS_\([A-Z0-9_]*\)}$/\1 \2/p' "$auth_conf" \
  | awk '{ print $1 " " tolower($2) }' | tr '_' '-')"

connections="$(printf '%s\n' "$connz" | awk '
  function flush() {
    if (cid != "")
      print cid "\t" (user == "" ? "-" : user) "\t" (ip == "" ? "-" : ip)
    cid = ""; user = ""; ip = ""
  }
  { text = text $0 "\n" }
  END {
    start = index(text, "\"connections\"")
    if (start == 0)
      exit
    text = substr(text, start)
    while (match(text, /"(cid|authorized_user|ip)":[[:space:]]*("[^"]*"|[0-9]+)/)) {
      token = substr(text, RSTART, RLENGTH)
      text = substr(text, RSTART + RLENGTH)
      key = token
      sub(/":.*$/, "", key)
      sub(/^"/, "", key)
      value = token
      sub(/^"[a-z_]*":[[:space:]]*/, "", value)
      gsub(/"/, "", value)
      if (key == "cid") {
        flush()
        cid = value
      } else if (key == "authorized_user") {
        user = value
      } else {
        ip = value
      }
    }
    flush()
  }
')"

is_row() {
  grep -qx "$1" <<<"$rows"
}

is_expected() {
  grep -qx "$1" <<<"$expected"
}

own=""
shared=""
unknown=""
connection_rows=""
count=0
while IFS=$'\t' read -r cid user ip; do
  [ -n "$cid" ] || continue
  count=$(( count + 1 ))
  if [ "$user" = "-" ]; then
    state="unknown"
    detail="no authorized_user"
    unknown="$unknown $cid"
  elif [ "$user" = "argus" ]; then
    state="SHARED"
    detail="the shared user"
    shared="$shared $cid"
  else
    service="$(awk -v key="$user" '$1 == key { print $2; exit }' <<<"$key_services")"
    if [ -z "$service" ]; then
      state="unknown"
      detail="$user is not in auth.conf"
      unknown="$unknown $cid"
    elif ! is_row "$service"; then
      state="unknown"
      detail="$user is the key of $service, which the permissions do not list"
      unknown="$unknown $cid"
    else
      state="$service"
      detail="$user"
      own="$own $service:$cid"
    fi
  fi
  connection_rows="$connection_rows$(printf '%-6s %-14s %-16s %s' "$cid" "$state" "$ip" "$detail")"$'\n'
done <<<"$connections"

missing=""
own_count=0
absent_count=0
printf 'nats-users-check: deployed services from %s\n' "$source_note"
printf '%-14s %-13s %s\n' "service" "state" "connections"
while read -r service; do
  [ -n "$service" ] || continue
  cids="$(tr ' ' '\n' <<<"$own" | awk -F ':' -v service="$service" '$1 == service { printf "%s%s", sep, $2; sep = " " }')"
  if ! is_expected "$service"; then
    absent_count=$(( absent_count + 1 ))
    printf '%-14s %-13s %s\n' "$service" "not deployed" \
      "not in the stack's active profiles${cids:+; cid $cids presented its key}"
  elif [ -n "$cids" ]; then
    own_count=$(( own_count + 1 ))
    printf '%-14s %-13s cid %s\n' "$service" "own" "$cids"
  else
    missing="$missing $service"
    if awk -v service="$service" '$2 == service { found = 1 } END { exit !found }' <<<"$key_services"; then
      printf '%-14s %-13s %s\n' "$service" "missing" "no connection presented its key"
    else
      printf '%-14s %-13s %s\n' "$service" "missing" "auth.conf holds no key for it; re-run provision-host.sh"
    fi
  fi
done <<<"$rows"

printf '\n%-6s %-14s %-16s %s\n' "cid" "state" "ip" "detail"
printf '%s' "$connection_rows"

expected_count="$(grep -c . <<<"$expected")"
printf 'nats-users-check: %s expected service(s), %s own, %s missing, %s not deployed; %s connection(s), %s SHARED, %s unknown\n' \
  "$expected_count" "$own_count" "$(wc -w <<<"$missing")" "$absent_count" "$count" "$(wc -w <<<"$shared")" \
  "$(wc -w <<<"$unknown")"

reasons=""
[ "$count" -gt 0 ] || reasons="$reasons; no client is connected, so bring the stack up and run it again"
[ -z "$missing" ] || reasons="$reasons; missing:$missing"
[ -z "$shared" ] || reasons="$reasons; SHARED connection(s), cid$shared"
[ -z "$unknown" ] || reasons="$reasons; unknown connection(s), cid$unknown"
if [ -n "$reasons" ]; then
  printf 'nats-users-check: fail%s; keep the shared user\n' "$reasons"
  exit 1
fi
printf 'nats-users-check: pass: every expected service has a connection of its own, none is SHARED or unknown\n'
