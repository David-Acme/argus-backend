#!/usr/bin/env bash
set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SYNC_SERVICE="${ROOT_DIR}/src/feature/socket/sync/services/sync-service.cc"

rg -q --fixed-strings "refreshContext(conn)" "${SYNC_SERVICE}"
rg -q --fixed-strings "User account is disabled" "${SYNC_SERVICE}"
rg -q --fixed-strings "ctx.role = user->role" "${SYNC_SERVICE}"

echo "role resync contract: PASS"
