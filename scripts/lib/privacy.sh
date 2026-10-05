PRIVACY_NOTICE_VERSION=1
PRIVACY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../privacy" && pwd)"
PRIVACY_ACCEPT="${ARGUS_ACCEPT_PRIVACY_NOTICE:-0}"
PRIVACY_VISITOR_ACCEPT="${ARGUS_ACCEPT_VISITOR_NOTICE:-0}"
PRIVACY_JURISDICTION="${ARGUS_PRIVACY_JURISDICTION:-pe}"
PRIVACY_LANG="${ARGUS_PRIVACY_LANG:-}"

privacy_default_lang() {
  case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
    en*) printf 'en' ;;
    *) printf 'es' ;;
  esac
}

privacy_lang() {
  local lang="${PRIVACY_LANG:-$(privacy_default_lang)}"
  case "$lang" in
    es|en) printf '%s' "$lang" ;;
    *) printf 'es' ;;
  esac
}

privacy_jurisdiction_row() {
  awk -F'\t' -v code="$1" 'NR > 1 && $1 == code { print; found = 1 } END { exit found ? 0 : 1 }' \
    "$PRIVACY_DIR/jurisdictions.tsv"
}

privacy_render() {
  local file="$1" lang="$2" row
  if ! row="$(privacy_jurisdiction_row "$PRIVACY_JURISDICTION")"; then
    err "Unknown privacy jurisdiction '$PRIVACY_JURISDICTION' (see scripts/privacy/jurisdictions.tsv)."
    return 2
  fi
  awk -v row="$row" -v lang="$lang" -v version="$PRIVACY_NOTICE_VERSION" '
    BEGIN {
      n = split(row, f, "\t")
      country = (lang == "en") ? f[3] : f[2]
      laws = (lang == "en") ? f[5] : f[4]
      authority = (lang == "en") ? f[7] : f[6]
    }
    {
      gsub(/\{\{VERSION\}\}/, version)
      gsub(/\{\{COUNTRY\}\}/, country)
      gsub(/\{\{LAWS\}\}/, laws)
      gsub(/\{\{AUTHORITY\}\}/, authority)
      gsub(/\{\{VIDEO_DAYS\}\}/, f[8])
      gsub(/\{\{VIDEO_MAX_DAYS\}\}/, f[9])
      gsub(/\{\{INCIDENT_DAYS\}\}/, f[10])
      print
    }' "$file"
}

privacy_notice_text() {
  local lang
  lang="$(privacy_lang)"
  privacy_render "$PRIVACY_DIR/notice.$lang.md" "$lang"
}

privacy_visitor_text() {
  local lang
  lang="$(privacy_lang)"
  privacy_render "$PRIVACY_DIR/visitors.$lang.md" "$lang"
}

privacy_record_path() {
  printf '%s/privacy/host-consent.json' "$1"
}

privacy_recorded_version() {
  local record="$1"
  [ -r "$record" ] || { printf '0'; return; }
  sed -n 's/.*"noticeVersion": *\([0-9][0-9]*\).*/\1/p' "$record" | head -1
}

privacy_recorded_visitors() {
  local record="$1"
  [ -r "$record" ] && grep -q '"visitorRecognitionAcknowledged": *true' "$record"
}

privacy_json_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

privacy_write_record() {
  local data_dir="$1" method="$2" visitors="$3" script="$4"
  local dir="$data_dir/privacy" record lang at by digest line
  record="$(privacy_record_path "$data_dir")"
  lang="$(privacy_lang)"
  at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  by="$(id -un 2>/dev/null || printf 'unknown')@$(uname -n 2>/dev/null || printf 'host')"
  digest="$(privacy_notice_text | openssl dgst -sha256 -r | awk '{print $1}')"
  mkdir -p "$dir"
  chmod 700 "$dir"
  line="$(printf '{"notice": "argus-host-privacy", "noticeVersion": %s, "termsVersion": %s, "jurisdiction": "%s", "language": "%s", "acceptedAt": "%s", "acceptedBy": "%s", "method": "%s", "script": "%s", "noticeSha256": "%s", "visitorRecognitionAcknowledged": %s}' \
    "$PRIVACY_NOTICE_VERSION" "$PRIVACY_NOTICE_VERSION" \
    "$(privacy_json_string "$PRIVACY_JURISDICTION")" "$lang" "$at" \
    "$(privacy_json_string "$by")" "$method" "$(privacy_json_string "$script")" \
    "$digest" "$visitors")"
  ( umask 077 && printf '%s\n' "$line" > "$record.tmp" && mv "$record.tmp" "$record" )
  ( umask 077 && printf '%s\n' "$line" >> "$dir/host-consent.log" )
  chmod 600 "$record" "$dir/host-consent.log"
}

privacy_prompt_word() {
  local expected="$1" answer
  read -r answer || return 1
  answer="$(printf '%s' "$answer" | tr '[:lower:]' '[:upper:]' | tr -d '[:space:]')"
  [ "$answer" = "$expected" ]
}

privacy_ask_visitors() {
  local lang word
  lang="$(privacy_lang)"
  if [ "$PRIVACY_VISITOR_ACCEPT" = 1 ]; then
    printf 'true'
    return
  fi
  if [ ! -t 0 ]; then
    printf 'false'
    return
  fi
  word="SI"
  [ "$lang" = en ] && word="YES"
  {
    echo
    privacy_visitor_text
    echo
    if [ "$lang" = en ]; then
      printf 'Type %s to acknowledge it now, or press Enter to skip (it stays off): ' "$word"
    else
      printf 'Escribe %s para aceptarlo ahora, o pulsa Enter para omitirlo (queda apagado): ' "$word"
    fi
  } >&2
  if privacy_prompt_word "$word"; then
    printf 'true'
  else
    printf 'false'
  fi
}

privacy_require_consent() {
  local data_dir="$1" script="$2" record lang word visitors method
  record="$(privacy_record_path "$data_dir")"
  lang="$(privacy_lang)"

  if [ "$(privacy_recorded_version "$record")" = "$PRIVACY_NOTICE_VERSION" ]; then
    if [ "$PRIVACY_VISITOR_ACCEPT" = 1 ] && ! privacy_recorded_visitors "$record"; then
      privacy_write_record "$data_dir" flag true "$script"
      log "Visitor recognition notice acknowledged and recorded in ${record}."
    fi
    log "Privacy notice v$PRIVACY_NOTICE_VERSION already accepted (${record})."
    return 0
  fi

  privacy_notice_text >&2 || return 2
  echo >&2

  if [ "$PRIVACY_ACCEPT" = 1 ]; then
    method="flag"
  elif [ -t 0 ]; then
    word="ACEPTO"
    [ "$lang" = en ] && word="ACCEPT"
    if [ "$lang" = en ]; then
      printf 'Type %s to accept this notice and continue (anything else cancels): ' "$word" >&2
    else
      printf 'Escribe %s para aceptar este aviso y continuar (cualquier otra cosa cancela): ' "$word" >&2
    fi
    if ! privacy_prompt_word "$word"; then
      if [ "$lang" = en ]; then
        err "Notice not accepted. Nothing was installed or configured."
      else
        err "Aviso no aceptado. No se instaló ni se configuró nada."
      fi
      exit 3
    fi
    method="interactive"
  else
    if [ "$lang" = en ]; then
      err "This run is not interactive: read the notice above and pass --accept-privacy-notice to accept it. Nothing was installed or configured."
    else
      err "Esta ejecución no es interactiva: lee el aviso de arriba y usa --accept-privacy-notice para aceptarlo. No se instaló ni se configuró nada."
    fi
    exit 3
  fi

  visitors="$(privacy_ask_visitors)"
  privacy_write_record "$data_dir" "$method" "$visitors" "$script"
  log "Privacy notice v$PRIVACY_NOTICE_VERSION accepted and recorded in ${record}."
}

privacy_withdraw_consent() {
  local data_dir="$1" record lang
  record="$(privacy_record_path "$data_dir")"
  lang="$(privacy_lang)"
  if [ -f "$record" ]; then
    ( umask 077 && printf '{"notice": "argus-host-privacy", "withdrawnAt": "%s", "withdrawnBy": "%s"}\n' \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
      "$(privacy_json_string "$(id -un 2>/dev/null || printf 'unknown')@$(uname -n 2>/dev/null || printf 'host')")" \
      >> "$data_dir/privacy/host-consent.log" )
    rm -f "$record"
  fi
  if [ "$lang" = en ]; then
    log "Acceptance withdrawn. The setup scripts will ask again before configuring anything."
    log "Stop Argus: (cd argus-deploy && docker compose down)   never add -v"
    log "Erase every piece of data: delete $data_dir after stopping Argus."
  else
    log "Aceptación retirada. Los scripts volverán a pedirla antes de configurar nada."
    log "Detén Argus: (cd argus-deploy && docker compose down)   nunca agregues -v"
    log "Para borrar todos los datos: elimina $data_dir después de detener Argus."
  fi
}
