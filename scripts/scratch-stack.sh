#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
usage: scripts/scratch-stack.sh <root> <stack> [options]
       scripts/scratch-stack.sh --down <root> <stack>
       scripts/scratch-stack.sh --check <root> <stack> [options]

Brings up a scratch native stack for a golden verify, from a tree that is built
and configured for scratch use. <root> must not be a Git work tree (a .git entry
is refused): the generated configs of <root> are rewritten for the scratch NATS
port, so the developer's own configs are never touched. Run it as the heavy job
it is, under the heavy-gate lock:
  flock -x -o ~/.cache/argus-phase2/heavy-one.lock \
    ~/.cache/argus-scratch-backup/heavy-gate.sh 8 \
    scripts/scratch-stack.sh <root> <stack> --with-ai

Steps, in order:
  1. Check <root>, the NATS port, the scratch NATS server binary, the binaries
     of the stack under <root>/services/<svc>/build/<profile>, and the models
     (and go2rtc) that the stack needs.
  2. Link --models and --go2rtc into <root> (a real models directory is refused).
  3. Point the NATS URL of <root>'s configs at the scratch port.
  4. Start a scratch JetStream broker on 127.0.0.1:<port> with its store under
     <stack>/nats, unless that broker is already up.
  5. scripts/native-stack.sh up with ARGUS_STACK_DIR=<stack>.
  6. With --with-ai: start llm, stt, tts and vlm from <stack> and wait for /health.
  7. Seed the golden rows, then the role sessions (seed-golden.py, plain then --roles).

--down stops the AI services, the native stack and the scratch broker of <stack>.

Options:
  --nats-port N      scratch broker port (default 14222; 4222 and 7025-7047 are refused)
  --with-ai          also start llm, stt, tts and vlm
  --models DIR       link <root>/models to DIR (required unless <root>/models exists)
  --go2rtc DIR       link <root>/third_party/go2rtc to DIR (required unless it exists)
  --check            validate every step and print the plan; change nothing

Environment:
  ARGUS_NATS_SERVER_BIN  path of the nats-server binary (required)
  ARGUS_STACK_PROFILE    build profile under services/<svc>/build (default dev)
USAGE
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

MODE=up
CHECK=0
NATS_PORT=14222
WITH_AI=0
MODELS=""
GO2RTC=""
PROFILE="${ARGUS_STACK_PROFILE:-dev}"
POSITIONAL=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --down) MODE=down ;;
    --check) CHECK=1 ;;
    --with-ai) WITH_AI=1 ;;
    --nats-port) shift; NATS_PORT="${1:?--nats-port needs a port}" ;;
    --models) shift; MODELS="${1:?--models needs a directory}" ;;
    --go2rtc) shift; GO2RTC="${1:?--go2rtc needs a directory}" ;;
    *) POSITIONAL+=("$1") ;;
  esac
  shift
done

if [ "${#POSITIONAL[@]}" -ne 2 ]; then
  usage >&2
  exit 2
fi

die() {
  echo "scratch-stack: $*" >&2
  exit 1
}

ROOT_ARG="${POSITIONAL[0]}"
STACK_ARG="${POSITIONAL[1]}"
test -d "$ROOT_ARG" || die "root $ROOT_ARG is not a directory"
ROOT="$(cd "$ROOT_ARG" && pwd)"
STACK="$(realpath -m "$STACK_ARG")"

NATIVE_SERVICES=(auth identity notification sync camera guard productivity settings)
AI_SERVICES=(llm stt tts vlm)
declare -A AI_PORT=([llm]=7032 [stt]=7030 [tts]=7029 [vlm]=7031)

if [ "$MODE" = down ]; then
  for pid in $(pgrep -f "build/$PROFILE/argus-(llm|stt|tts|vlm)" || true); do
    cwd="$(readlink "/proc/$pid/cwd" 2>/dev/null || true)"
    case "$cwd" in
      "$STACK"/*) kill "$pid" ;;
    esac
  done
  ARGUS_STACK_DIR="$STACK" "$ROOT/scripts/native-stack.sh" down || true
  if [ -f "$STACK/nats/pid" ]; then
    kill "$(cat "$STACK/nats/pid")" 2>/dev/null || true
    rm -f "$STACK/nats/pid"
  fi
  echo "scratch stack down: $STACK"
  exit 0
fi

test -e "$ROOT/.git" && die "$ROOT is a Git work tree; the scratch stack rewrites its generated configs, so it refuses it"

case "$NATS_PORT" in
  ''|*[!0-9]*) die "--nats-port must be a number, got '$NATS_PORT'" ;;
esac
if [ "$NATS_PORT" -eq 4222 ] || { [ "$NATS_PORT" -ge 7025 ] && [ "$NATS_PORT" -le 7047 ]; }; then
  die "NATS port $NATS_PORT belongs to the production broker or the native services; choose another"
fi

test -n "${ARGUS_NATS_SERVER_BIN:-}" && test -x "$ARGUS_NATS_SERVER_BIN" \
  || die "ARGUS_NATS_SERVER_BIN must name an executable nats-server"

stack_services=("${NATIVE_SERVICES[@]}")
if [ "$WITH_AI" -eq 1 ]; then
  stack_services+=("${AI_SERVICES[@]}")
fi
for svc in "${stack_services[@]}"; do
  binary="$ROOT/services/$svc/build/$PROFILE/argus-$svc"
  test -x "$binary" || die "missing binary $binary (build the $svc project for profile $PROFILE)"
done

if [ -n "$MODELS" ]; then
  test -d "$MODELS" || die "--models $MODELS is not a directory"
  if [ -e "$ROOT/models" ] && [ ! -L "$ROOT/models" ]; then
    die "$ROOT/models is a real directory; it is not replaced"
  fi
fi
if [ -z "$MODELS" ]; then
  test -d "$ROOT/models" || die "$ROOT/models is missing (pass --models DIR)"
fi

if [ -n "$GO2RTC" ]; then
  test -d "$GO2RTC" || die "--go2rtc $GO2RTC is not a directory"
fi
if [ ! -e "$ROOT/third_party/go2rtc" ] && [ -z "$GO2RTC" ]; then
  die "$ROOT/third_party/go2rtc is missing (pass --go2rtc DIR)"
fi

echo "plan for $ROOT -> $STACK"
echo "  nats: 127.0.0.1:$NATS_PORT (store $STACK/nats/store)"
echo "  native services: ${NATIVE_SERVICES[*]}"
if [ "$WITH_AI" -eq 1 ]; then
  echo "  ai services: ${AI_SERVICES[*]}"
fi
if [ "$CHECK" -eq 1 ]; then
  echo "check passed: nothing changed"
  exit 0
fi

if [ -n "$MODELS" ]; then
  ln -sfn "$MODELS" "$ROOT/models"
fi
if [ -n "$GO2RTC" ]; then
  if [ -d "$ROOT/third_party/go2rtc" ] && [ ! -L "$ROOT/third_party/go2rtc" ]; then
    rmdir "$ROOT/third_party/go2rtc" 2>/dev/null \
      || die "$ROOT/third_party/go2rtc is a non-empty directory; it is not replaced"
  fi
  mkdir -p "$ROOT/third_party"
  ln -sfn "$GO2RTC" "$ROOT/third_party/go2rtc"
fi

for config in "$ROOT"/services/*/config.toml; do
  if grep -q 'nats://127.0.0.1:4222' "$config"; then
    sed -i "s#nats://127.0.0.1:4222#nats://127.0.0.1:$NATS_PORT#" "$config"
  fi
done

mkdir -p "$STACK/nats/store" "$STACK/logs"
broker_up() {
  (exec 3<>"/dev/tcp/127.0.0.1/$NATS_PORT") 2>/dev/null
}
if broker_up; then
  test -f "$STACK/nats/pid" && kill -0 "$(cat "$STACK/nats/pid")" 2>/dev/null \
    || die "port $NATS_PORT is busy and is not this stack's broker"
else
  printf 'listen: 127.0.0.1:%s\njetstream: {store_dir: "%s/nats/store"}\n' \
    "$NATS_PORT" "$STACK" > "$STACK/nats/nats.conf"
  (
    cd "$STACK/nats" && setsid --fork "$ARGUS_NATS_SERVER_BIN" -c "$STACK/nats/nats.conf" \
      >> "$STACK/logs/nats.log" 2>&1 < /dev/null
  )
  for _ in $(seq 1 20); do
    broker_up && break
    sleep 0.5
  done
  broker_up || die "the scratch broker did not listen on $NATS_PORT (see $STACK/logs/nats.log)"
  pgrep -f "nats-server.*$STACK/nats/nats.conf" | head -1 > "$STACK/nats/pid"
fi

ARGUS_STACK_DIR="$STACK" "$ROOT/scripts/native-stack.sh" up

if [ "$WITH_AI" -eq 1 ]; then
  for svc in "${AI_SERVICES[@]}"; do
    mkdir -p "$STACK/$svc/database"
    cp "$ROOT/services/$svc/config.toml" "$STACK/$svc/config.toml"
    ln -sfn "$ROOT/models" "$STACK/$svc/models"
    (
      cd "$STACK/$svc" && setsid --fork "$ROOT/services/$svc/build/$PROFILE/argus-$svc" \
        >> "$STACK/logs/$svc.log" 2>&1 < /dev/null
    )
  done
  for svc in "${AI_SERVICES[@]}"; do
    healthy=0
    for _ in $(seq 1 240); do
      if curl -sf "http://127.0.0.1:${AI_PORT[$svc]}/health" >/dev/null 2>&1; then
        healthy=1
        break
      fi
      sleep 2
    done
    test "$healthy" -eq 1 || die "$svc did not answer /health on ${AI_PORT[$svc]}"
  done
fi

python3 -I "$ROOT/scripts/seed-golden.py" --stack-dir "$STACK"
python3 -I "$ROOT/scripts/seed-golden.py" --stack-dir "$STACK" --roles

echo "scratch stack ready: $STACK"
echo "verify with: python3 -I $ROOT/scripts/golden-http.py verify --stack-dir $STACK"
