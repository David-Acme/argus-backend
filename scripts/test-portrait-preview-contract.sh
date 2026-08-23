#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

assert_contains() {
  local file="$1"
  local pattern="$2"
  rg -q --fixed-strings "$pattern" "$root/$file"
}

assert_contains "database/schema.sql" "CREATE TABLE IF NOT EXISTS portrait_preview_capability"
assert_contains "src/feature/api/user/controllers/portrait-preview-controller.hxx" '"/portrait-preview/{1}"'
assert_contains "src/feature/api/user/controllers/portrait-preview-controller.hxx" '"/portrait-preview/{1}/content"'
assert_contains "src/feature/api/user/services/portrait-preview-service.cc" "tryConsume"
assert_contains "src/feature/api/user/services/portrait-preview-service.cc" "UserAction::Read"
assert_contains "src/shared/services/storage/private-portrait-service.hxx" "read(int64_t userId)"
assert_contains "src/shared/services/storage/s3-storage-service.hxx" "get(const std::string& objectKey)"

echo "portrait preview contract: PASS"
