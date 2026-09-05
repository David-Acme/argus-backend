#!/usr/bin/env bash
# Identity route probe matrix for the argus_identity extraction (F1-3b).
#
# Runs every identity route against one server (backend or gateway) and writes
# one normalized capture file per probe. Used for two reviews:
#   1. A/B byte-identity of the legacy backend before/after the extraction
#      (unauthenticated probes only, two runs, `diff` of the capture trees).
#   2. Envelope parity between the gateway (identity.db) and the backend
#      (argus.db), seeded identically (includes the authenticated and
#      mutation probes: --jwt + --mutations).
#
# Captures are normalized: random tokens, challenge ids and timestamps are
# replaced by placeholders so two runs of the same route diff to zero when the
# wire behavior is identical.
#
# Usage:
#   probe-identity-matrix.sh --base https://127.0.0.1:7044 --out /tmp/be \
#     [--jwt <access-token>] [--ua <user-agent>] [--image <face.jpg>]
#     [--mutations] [--label backend]
set -euo pipefail

BASE=""
OUT=""
JWT=""
UA="argus-probe/1.0"
IMAGE="/tmp/face-test.jpg"
MUTATIONS=0
PROXIED=0
LABEL="probe"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --jwt) JWT="$2"; shift 2 ;;
    --ua) UA="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --mutations) MUTATIONS=1; shift ;;
    --proxied) PROXIED=1; shift ;;
    --label) LABEL="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$BASE" && -n "$OUT" ]] || { echo "--base and --out are required" >&2; exit 2; }
mkdir -p "$OUT"

normalize() {
  python3 -c '
import json, re, sys
raw = sys.stdin.read()
def scrub(node):
    if isinstance(node, dict):
        out = {}
        for key, value in node.items():
            if key in ("token", "inviteCode", "challengeId", "accessToken",
                       "refreshToken", "capability"):
                out[key] = "<redacted>"
            elif key in ("createdAt", "updatedAt", "expiresAt", "revokedAt",
                         "redeemedAt", "consumedAt", "expiresIn", "syncAt",
                         "uptimeSeconds", "eventTimestamp"):
                out[key] = "<timestamp>"
            else:
                out[key] = scrub(value)
        return out
    if isinstance(node, list):
        return [scrub(item) for item in node]
    return node
try:
    print(json.dumps(scrub(json.loads(raw)), indent=1, sort_keys=True))
except (ValueError, TypeError):
    print(raw.strip())
'
}

# probe <id> <method> <path> [curl extra args...]
probe() {
  local id="$1"; shift
  local method="$1"; shift
  local path="$1"; shift
  local file="$OUT/$(printf '%02d' "$id")-$method-$(echo "$path" | tr '/' '_').txt"
  mkdir -p "$OUT/raw"
  curl -sk --max-time 30 -X "$method" "$BASE$path" "${UAARG[@]}" "$@" \
    -o /tmp/probe-body.$$ -D /tmp/probe-head.$$ \
    -w 'HTTP-STATUS %{http_code}\n' > "$file" 2>&1 \
    || echo "HTTP-STATUS curl-error" >> "$file"
  grep -iE '^(content-type|access-control-allow-origin|access-control-allow-methods|access-control-allow-headers|allow):' \
    /tmp/probe-head.$$ >> "$file" 2>/dev/null || true
  {
    echo "-- body"
    normalize < /tmp/probe-body.$$
  } >> "$file"
  cp /tmp/probe-body.$$ "$OUT/raw/$(printf '%02d' "$id").json"
  rm -f /tmp/probe-body.$$ /tmp/probe-head.$$
  echo "wrote $file"
}

AUTH=()
[[ -n "$JWT" ]] && AUTH=(-H "Authorization: Bearer $JWT")
UAARG=(-A "$UA")

# ── Unauthenticated matrix: every identity route, validation, 404, 405, CORS ──
probe 1 GET  /health
probe 2 POST /auth/login
probe 3 POST /auth/register
probe 4 GET  /auth/status
probe 5 POST /auth/device-login
probe 6 POST /auth/device-login/00000000deadbeef/approve
probe 7 GET  /auth/device-login/00000000deadbeef
probe 8 PATCH /auth/refresh-token -H 'Content-Type: application/json' -d '{"refreshToken":"no-such-token"}'
probe 9 PATCH /auth/logout
probe 10 PATCH /auth/me "${AUTH[@]}" -H 'Content-Type: application/json' -d '{}'
probe 11 POST /invitation/resolve -H 'Content-Type: application/json' -d '{}'
probe 12 GET  /invitation "${AUTH[@]}"
probe 13 POST /invitation "${AUTH[@]}" -H 'Content-Type: application/json' -d '{}'
probe 14 DELETE /invitation/999 "${AUTH[@]}"
probe 15 GET  /user "${AUTH[@]}"
probe 16 PATCH /user/1 "${AUTH[@]}" -H 'Content-Type: application/json' -d '{}'
probe 17 DELETE /user/999 "${AUTH[@]}"
probe 18 GET  /portrait-preview/1 "${AUTH[@]}"
probe 19 GET  /portrait-preview/1/content "${AUTH[@]}"
probe 20 POST /pairing -H 'Content-Type: application/json' -d '{"caCert":"x"}'
probe 21 GET  /invitation
probe 22 GET  /no-such-route
probe 23 PATCH /user
probe 24 OPTIONS /user

# ── Proxied matrix: legacy-owned routes only (F1-5 cutover). ────────────────
# Stateless probes so two runs (through the gateway and direct against the
# legacy) diff to zero; gateway-native paths are never sent here because
# Ruling I forbids the proxy from serving them.
if [[ "$PROXIED" == "1" ]]; then
  probe 50 GET /camera/1/status "${AUTH[@]}"
  probe 51 GET /camera/999/status "${AUTH[@]}"
  probe 52 GET /camera/0/status "${AUTH[@]}"
  probe 53 GET /camera/1/presets "${AUTH[@]}"
  probe 54 POST /camera -H 'Content-Type: application/json' -d '{}'
  probe 55 POST /zone -H 'Content-Type: application/json' -d '{}'
  probe 56 PATCH /camera/999/settings "${AUTH[@]}" -H 'Content-Type: application/json' -d '{}'
  probe 57 DELETE /zone/999 "${AUTH[@]}"
  probe 58 POST /calendar-event -H 'Content-Type: application/json' -d '{}'
  probe 59 POST /notification/read -H 'Content-Type: application/json' -d '{}'
  # Multipart fidelity through the proxy: the same multipart body must reach
  # the legacy unchanged (identical error envelope direct vs proxied).
  probe 60 POST /camera -F "part=@$IMAGE;type=image/jpeg"
  probe 61 OPTIONS /camera
  probe 62 GET /no-such-route
fi

if [[ "$MUTATIONS" != "1" ]]; then
  echo "done (unauthenticated matrix only)"
  exit 0
fi
[[ -n "$JWT" ]] || { echo "--mutations requires --jwt" >&2; exit 2; }

# ── Authenticated happy-path and mutation sequence (mutates server state) ──
probe 30 GET   /user "${AUTH[@]}"
probe 31 GET   /auth/status "${AUTH[@]}"
probe 32 GET   /invitation "${AUTH[@]}"

FUTURE=$(( $(date +%s) + 86400 ))
probe 33 POST  /invitation "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d "{\"role\":\"guest\",\"maxRedemptions\":2,\"expiresAt\":$FUTURE}"

probe 34 PATCH /auth/me "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"name":"Ada Probe"}'
probe 35 PATCH /user/1 "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"lastName":"Lovelace"}'
probe 36 GET   /portrait-preview/1 "${AUTH[@]}"

INVITE_TOKEN=$(python3 -c '
import json, sys
try:
    print(json.load(open(sys.argv[1]))["info"]["token"])
except (OSError, KeyError, ValueError):
    print("")
' "$OUT/raw/33.json" 2>/dev/null || true)

probe 37 POST  /auth/register "${AUTH[@]}" \
  -F "image=@$IMAGE;type=image/jpeg" \
  -F "name=Grace Hopper" \
  -F "lang=en" \
  -F "inviteCode=$INVITE_TOKEN"

probe 38 POST  /auth/login "${AUTH[@]}" -F "image=@$IMAGE;type=image/jpeg"
probe 39 PATCH /user/2 "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"isActive":false}'
probe 40 DELETE /invitation/1 "${AUTH[@]}"
probe 41 DELETE /user/2 "${AUTH[@]}"
probe 42 GET   /user "${AUTH[@]}"
probe 43 PATCH /auth/logout "${AUTH[@]}"
probe 44 PATCH /auth/refresh-token "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"refreshToken":"f1fix-refresh"}'
probe 45 GET   /auth/status "${AUTH[@]}"

echo "done (full matrix with mutations)"
