#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

fail() {
  printf 'privacy-consent-test: %s\n' "$*" >&2
  exit 1
}

consent() {
  local data_dir="$1"
  shift
  env "$@" bash -c '
    set -euo pipefail
    source "$0/scripts/lib/common.sh"
    source "$0/scripts/lib/privacy.sh"
    privacy_require_consent "$1" "test"
  ' "$ROOT" "$data_dir"
}

withdraw() {
  bash -c '
    set -euo pipefail
    source "$0/scripts/lib/common.sh"
    source "$0/scripts/lib/privacy.sh"
    privacy_withdraw_consent "$1"
  ' "$ROOT" "$1"
}

record() {
  cat "$1/privacy/host-consent.json"
}

for lang in es en; do
  text="$(ARGUS_PRIVACY_LANG="$lang" "$ROOT/scripts/setup.sh" --show-privacy-notice)"
  grep -q '29733' <<<"$text" || fail "$lang notice does not quote the jurisdiction's law"
  grep -q '120' <<<"$text" || fail "$lang notice does not quote the incident retention"
  if grep -q '{{' <<<"$text"; then
    fail "$lang notice left a placeholder unrendered"
  fi
done

refused="$TEST_TMP/refused"
if consent "$refused" ARGUS_PRIVACY_LANG=en </dev/null 2>"$TEST_TMP/err"; then
  fail "a non-interactive run without the flag was accepted"
fi
grep -q 'accept-privacy-notice' "$TEST_TMP/err" || fail "the refusal does not name the flag"
[ ! -e "$refused/privacy/host-consent.json" ] || fail "a refused run wrote a record"

if ARGUS_ASSUME_YES=1 consent "$refused" </dev/null 2>/dev/null; then
  fail "-y / ARGUS_ASSUME_YES accepted the notice"
fi

if ARGUS_PRIVACY_JURISDICTION=zz bash -c 'source "$0/scripts/lib/common.sh"; source "$0/scripts/lib/privacy.sh"; privacy_notice_text' "$ROOT" >/dev/null 2>&1; then
  fail "an unknown jurisdiction rendered a notice"
fi

accepted="$TEST_TMP/accepted"
consent "$accepted" ARGUS_ACCEPT_PRIVACY_NOTICE=1 ARGUS_PRIVACY_LANG=es </dev/null 2>/dev/null \
  || fail "the flag did not accept the notice"
json="$(record "$accepted")"
grep -q '"noticeVersion": 1' <<<"$json" || fail "the record lacks the version"
grep -q '"termsVersion": 1' <<<"$json" || fail "the record lacks the terms version"
grep -q '"method": "flag"' <<<"$json" || fail "the record lacks the method"
grep -q '"jurisdiction": "pe"' <<<"$json" || fail "the record lacks the jurisdiction"
grep -q '"visitorRecognitionAcknowledged": false' <<<"$json" || fail "visitors were acknowledged by default"
grep -Eq '"acceptedAt": "[0-9]{4}-[0-9]{2}-[0-9]{2}T' <<<"$json" || fail "the record lacks the time"
grep -q '"acceptedBy": ".*@' <<<"$json" || fail "the record lacks who accepted"
grep -Eq '"noticeSha256": "[0-9a-f]{64}"' <<<"$json" || fail "the record lacks the notice digest"
[ "$(stat -c %a "$accepted/privacy/host-consent.json")" = 600 ] || fail "the record is not 0600"
[ "$(stat -c %a "$accepted/privacy")" = 700 ] || fail "the record folder is not 0700"

consent "$accepted" ARGUS_PRIVACY_LANG=es </dev/null 2>"$TEST_TMP/again" \
  || fail "an accepted notice was asked again"
if grep -q 'AVISO DE PRIVACIDAD' "$TEST_TMP/again"; then
  fail "an accepted notice was printed again"
fi

consent "$accepted" ARGUS_ACCEPT_VISITOR_NOTICE=1 </dev/null 2>/dev/null \
  || fail "the visitor acknowledgement failed"
grep -q '"visitorRecognitionAcknowledged": true' <<<"$(record "$accepted")" \
  || fail "the visitor acknowledgement was not recorded"
[ "$(wc -l < "$accepted/privacy/host-consent.log")" -eq 2 ] || fail "the log does not keep every acceptance"

withdraw "$accepted" >/dev/null 2>&1
[ ! -e "$accepted/privacy/host-consent.json" ] || fail "withdrawal kept the record"
grep -q '"withdrawnAt"' "$accepted/privacy/host-consent.log" || fail "withdrawal was not logged"
if consent "$accepted" </dev/null 2>/dev/null; then
  fail "after withdrawal a non-interactive run was accepted"
fi

if command -v script >/dev/null 2>&1; then
  typed="$TEST_TMP/typed"
  printf 'acepto\n\n' | script -qec "ARGUS_PRIVACY_LANG=es bash -c 'source \"$ROOT/scripts/lib/common.sh\"; source \"$ROOT/scripts/lib/privacy.sh\"; privacy_require_consent \"$typed\" test'" /dev/null >/dev/null \
    || fail "typing ACEPTO did not accept"
  grep -q '"method": "interactive"' <<<"$(record "$typed")" || fail "the interactive method was not recorded"

  declined="$TEST_TMP/declined"
  if printf 'si\n' | script -qec "ARGUS_PRIVACY_LANG=es bash -c 'source \"$ROOT/scripts/lib/common.sh\"; source \"$ROOT/scripts/lib/privacy.sh\"; privacy_require_consent \"$declined\" test'" /dev/null >/dev/null; then
    [ ! -e "$declined/privacy/host-consent.json" ] || fail "a word other than ACEPTO was accepted"
  fi
  [ ! -e "$declined/privacy/host-consent.json" ] || fail "a word other than ACEPTO wrote a record"
fi

printf 'privacy-consent-test: ok\n'
