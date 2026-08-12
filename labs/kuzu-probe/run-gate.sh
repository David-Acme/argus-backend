#!/usr/bin/env bash
# Phase 0 gate runner — ejecuta todos los casos de labs/kuzu-probe y deja el
# log en /tmp/argus-kuzu-gate.log. Uso:
#   bash labs/kuzu-probe/run-gate.sh           (todo, incluye Release)
#   bash labs/kuzu-probe/run-gate.sh --debug   (solo Debug, ~15 min)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PROBE_DEV="$ROOT/build/dev/labs/kuzu-probe/argus-kuzu-probe"
LOG=/tmp/argus-kuzu-gate.log
DB="$ROOT/database/kuzu-probe.kz"

cleanup() {
  pkill -9 -f argus-kuzu-probe 2>/dev/null
  rm -rf "$DB" "$DB.wal" "$DB.lock"
  sleep 1
}

case_run() {
  local name="$1" timeout_s="$2"; shift 2
  cleanup
  echo "===== CASE: $name ($(date -u +%H:%M:%S)) =====" >> "$LOG"
  ( cd "$ROOT/build/dev/labs/kuzu-probe" && timeout "$timeout_s" "$PROBE_DEV" "$@" ) >> "$LOG" 2>&1
  echo "rc=$? (124=timeout/colgado, 134=assert/core)" >> "$LOG"
  echo "" >> "$LOG"
}

rm -f "$LOG"
echo "Argus kuzu gate log — $(date -u)" > "$LOG"

case_run "gate completo 5000 facts (con worker)" 300 --facts 5000
case_run "sin worker 5000 facts" 300 --facts 5000 --no-load
case_run "seq match (insert+MATCH plano)" 120 --facts 100 --seq-load --seq-query match
case_run "seq knn (insert+KNN)" 120 --facts 100 --seq-load --seq-query knn
case_run "seq fts (insert+FTS)" 120 --facts 100 --seq-load --seq-query fts
case_run "seq trav (insert+traversal)" 120 --facts 100 --seq-load --seq-query trav
case_run "seq mix (insert+mezcla)" 120 --facts 100 --seq-load --seq-query mix
case_run "worker thread real batch=20" 120 --facts 100 --writer-batch 20

if [ "${1:-}" != "--debug" ]; then
  echo "===== CASE: build prod (Release) =====" >> "$LOG"
  ( cd "$ROOT" && cmake --build --preset prod -j 8 --target argus-kuzu-probe ) >> "$LOG" 2>&1
  echo "build rc=$?" >> "$LOG"
  cleanup
  echo "===== CASE: gate Release 5000 facts =====" >> "$LOG"
  ( cd "$ROOT/build/prod/labs/kuzu-probe" && timeout 600 ./argus-kuzu-probe --facts 5000 ) >> "$LOG" 2>&1
  echo "rc=$?" >> "$LOG"
fi

cleanup
echo ""
echo "LOG: $LOG"
echo "Pásame el contenido completo de $LOG"
