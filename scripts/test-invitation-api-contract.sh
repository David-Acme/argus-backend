#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

assert_contains() {
  local file="$1"
  local pattern="$2"
  rg -q --fixed-strings "$pattern" "$root/$file"
}

# This contract intentionally checks the public API surface without requiring
# the AI services or an app process. It is the fast regression guard for the
# invitation QR handshake and owner-only management endpoints.
assert_contains "src/feature/api/invitation/controllers/invitation-controller.hxx" '"/invitation/resolve"'
assert_contains "src/feature/api/invitation/controllers/invitation-controller.hxx" '"/invitation"'
assert_contains "src/feature/api/invitation/controllers/invitation-controller.hxx" '"/invitation/{1}"'
assert_contains "src/feature/api/user/controllers/user-controller.hxx" '"/user"'
assert_contains "src/feature/api/user/controllers/user-controller.hxx" '"/user/{1}"'
assert_contains "src/feature/api/invitation/services/invitation-feature-service.cc" "CertService::caPem()"
assert_contains "src/feature/api/invitation/services/invitation-feature-service.cc" "CertService::isLoaded()"
assert_contains "src/feature/api/auth/services/auth-service.cc" "newTransactionCoro("
assert_contains "src/feature/api/auth/services/auth-service.cc" "TransactionType::Immediate"
assert_contains "src/feature/api/auth/services/auth-service.cc" "socketService_.emitModule(TableName::User, emit)"
assert_contains "src/feature/api/user/services/user-feature-service.cc" "socketService_.replaceRoleRooms"
assert_contains "src/feature/api/user/services/user-feature-service.cc" "SyncOperation::AuthContextChanged"

echo "invitation API contract: PASS"
