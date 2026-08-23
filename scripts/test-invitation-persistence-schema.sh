#!/usr/bin/env bash

set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SCHEMA_FILE="${ROOT_DIR}/database/schema.sql"
readonly SQLITE_BIN="${SQLITE_BIN:-sqlite3}"

for table in user_invitation invitation_redemption stored_file user_portrait \
  portrait_access_request portrait_access_grant; do
  grep -Fq "CREATE TABLE IF NOT EXISTS ${table}" "${SCHEMA_FILE}"
done

tmp_db="$(mktemp "${TMPDIR:-/tmp}/argus-invitation-schema.XXXXXX.db")"
tmp_schema="$(mktemp "${TMPDIR:-/tmp}/argus-invitation-schema.XXXXXX.sql")"
trap 'rm -f "${tmp_db}" "${tmp_schema}"' EXIT

# The SDK sqlite CLI intentionally has no FTS5 module. The application registers
# its own SQLite extensions, so this validates every ordinary table and index.
awk '
  /-- ── Virtual tables/ { skip = 1; next }
  /-- ── Indexes/ { skip = 0 }
  !skip { print }
' "${SCHEMA_FILE}" > "${tmp_schema}"

"${SQLITE_BIN}" "${tmp_db}" < "${tmp_schema}" >/dev/null

for table in user_invitation invitation_redemption stored_file user_portrait \
  portrait_access_request portrait_access_grant; do
  actual="$("${SQLITE_BIN}" "${tmp_db}" \
    "SELECT name FROM sqlite_master WHERE type = 'table' AND name = '${table}';")"
  test "${actual}" = "${table}"
done

for index in idx_user_invitation_token_hash idx_invitation_redemption_user \
  idx_stored_file_object_key idx_user_portrait_user_current \
  idx_portrait_access_request_owner_status idx_portrait_access_grant_lookup; do
  actual="$("${SQLITE_BIN}" "${tmp_db}" \
    "SELECT name FROM sqlite_master WHERE type = 'index' AND name = '${index}';")"
  test "${actual}" = "${index}"
done

"${SQLITE_BIN}" "${tmp_db}" <<'SQL'
PRAGMA foreign_keys = ON;
INSERT INTO user (name, last_name, role) VALUES ('Owner', 'One', 'owner');
INSERT INTO user_invitation (token_hash, role, max_redemptions, expires_at, created_by)
VALUES ('token-hash', 'resident', 1, 4102444800, 1);
INSERT INTO invitation_redemption (invitation_id, user_id) VALUES (1, 1);
INSERT INTO stored_file (object_key, sha256, mime_type, byte_size, category, created_by)
VALUES ('portraits/1.jpg', 'hash', 'image/jpeg', 1, 'portrait', 1);
INSERT INTO user_portrait (user_id, file_id) VALUES (1, 1);
INSERT INTO portrait_access_request (portrait_user_id, requester_user_id, status)
VALUES (1, 1, 'pending');
INSERT INTO portrait_access_grant
    (portrait_user_id, grantee_user_id, granted_by, scope, expires_at)
VALUES (1, 1, 1, 'temporary', 4102444800);
SQL

echo "Invitation/private portrait schema contract passed."
