#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

assert_contains() {
  local file="$1"
  local pattern="$2"
  rg -q --fixed-strings "$pattern" "$root/$file"
}

# People data is a role-scoped sync projection. Invitation secrets must never
# become a sync field, while guards receive the user directory.
assert_contains "src/shared/enums.hxx" "UserInvitation"
assert_contains "src/shared/enums.hxx" '"user_invitation"'
assert_contains "src/shared/access/role-access.hxx" "{TableName::User, kRead}"
assert_contains "src/feature/socket/sync/services/synchronized-service.cc" "TableName::UserInvitation"
assert_contains "src/feature/socket/sync/dtos/synchronized-dto.hxx" "userInvitation"
assert_contains "src/shared/repositories/user-invitation/user-invitation-repository.hxx" "public Syncable"
assert_contains "src/shared/repositories/user-invitation/user-invitation-repository.cc" "UserInvitationSchema(row).toJson()"
assert_contains "src/shared/repositories/user-invitation/user-invitation-query.hxx" "COALESCE(updated_at, created_at)"
assert_contains "src/shared/repositories/user/user-query.hxx" "COALESCE(updated_at, created_at)"
assert_contains "src/shared/schemas/user-invitation/user-invitation-schema.cc" 'value["syncAt"]'
# HTTP listing follows the same scope as sync: guards and owners receive the
# directory; every other role is confined to its own user row.
assert_contains "src/feature/api/user/services/user-feature-service.hxx" "int64_t actorId, UserRole actorRole"
assert_contains "src/feature/api/user/services/user-feature-service.cc" "actorRole == UserRole::Guard"
assert_contains "src/feature/api/user/services/user-feature-service.cc" "findById(actorId)"

# Sync projection serialization cannot leak the stored hash or raw QR token.
if rg -q --fixed-strings 'value["tokenHash"]' \
  "$root/src/shared/schemas/user-invitation/user-invitation-schema.cc"; then
  echo "user invitation sync leaked tokenHash" >&2
  exit 1
fi

echo "people sync contract: PASS"
