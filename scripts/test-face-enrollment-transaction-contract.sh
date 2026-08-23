#!/usr/bin/env bash
set -euo pipefail

auth='src/feature/api/auth/services/auth-service.cc'

if ! rg -q 'transaction->setCommitCallback' "$auth" ||
   ! rg -q 'FaceService::instance\(\)\.faceDb\(\)\.insert' "$auth"; then
  echo 'face vector must be indexed after the enrollment transaction commits'
  exit 1
fi

if rg -q 'INSERT_FACE_VEC' "$auth"; then
  echo 'face vector must use the binary vector connection, not the ORM binder'
  exit 1
fi

if ! rg -q 'indexFuture->get\(\)' "$auth"; then
  echo 'enrollment must wait for its face index before issuing a session'
  exit 1
fi

echo 'Face enrollment transaction contract passed.'
