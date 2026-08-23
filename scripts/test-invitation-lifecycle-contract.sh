#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

assert_contains() {
  local file="$1"
  local pattern="$2"
  rg -q --fixed-strings "$pattern" "$root/$file"
}

# A QR survives only while its in-memory preview is open. Its metadata still
# reaches the owner projection, while its opaque token never does.
assert_contains "src/feature/api/invitation/services/invitation-feature-service.cc" \
  "socketService_.emitModule(TableName::UserInvitation"
assert_contains "src/feature/api/invitation/services/invitation-feature-service.cc" \
  "userActionLogService_.record"
assert_contains "src/feature/api/auth/services/auth-service.cc" \
  "TableName::UserInvitation"
assert_contains "../frontend/src/app/users/index.tsx" \
  "inviteService.revoke(preview.invitationId)"

echo "invitation lifecycle contract: PASS"
