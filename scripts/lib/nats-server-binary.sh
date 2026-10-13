nats_server_binary_find() {
  local server="${ARGUS_NATS_SERVER_BIN:-}"
  if [ -n "$server" ] && [ -f "$server" ] && [ -x "$server" ]; then
    NATS_SERVER_BIN="$server"
    return 0
  fi
  NATS_SERVER_MISSING="ARGUS_NATS_SERVER_BIN is not set"
  [ -z "$server" ] || NATS_SERVER_MISSING="ARGUS_NATS_SERVER_BIN ($server) names no executable file"
  return 1
}

nats_server_binary_fail_in_ci() {
  local name="$1"
  [ "${CI:-}" = "true" ] || return 0
  printf '%s: failed, %s, and with CI=true a missing nats-server is a failure, not a skip\n' \
    "$name" "$NATS_SERVER_MISSING" >&2
  exit 1
}

nats_server_binary_or_exit() {
  local name="$1"
  nats_server_binary_find && return 0
  nats_server_binary_fail_in_ci "$name"
  printf '%s: skipped, %s\n' "$name" "$NATS_SERVER_MISSING"
  exit 77
}

nats_scratch_port() {
  local name="$1" candidate
  for _ in $(seq 1 50); do
    candidate=$(( 20000 + RANDOM % 40000 ))
    [ "$candidate" -ne 4222 ] || continue
    if ! (exec 3<>"/dev/tcp/127.0.0.1/$candidate") 2>/dev/null; then
      NATS_SCRATCH_PORT="$candidate"
      break
    fi
  done
  [ -n "${NATS_SCRATCH_PORT:-}" ] || { printf '%s: no free port\n' "$name" >&2; exit 1; }
  [ "$NATS_SCRATCH_PORT" -ne 4222 ] || { printf '%s: port 4222 belongs to the stack broker, refusing it\n' "$name" >&2; exit 1; }
}

nats_scratch_wait() {
  local name="$1" pid="$2" port="$3" log="$4"
  for _ in $(seq 1 200); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
      return 0
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.05
  done
  cat "$log" >&2
  printf '%s: the broker did not start\n' "$name" >&2
  exit 1
}
