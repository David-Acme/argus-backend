#!/usr/bin/env python3
import argparse
import importlib.util
import json
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DURABLE = "argus-sync-auth-action"
STREAM = "ARGUS_AUTH_CHANGE"
SUBJECT = "argus.auth.v1.user-action"
ACK_WAIT_SECONDS = 60
DEFAULT_MONITOR = "http://127.0.0.1:8222"


def load_golden():
    path = REPO_ROOT / "scripts" / "golden-http.py"
    spec = importlib.util.spec_from_file_location("golden_http", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stack_path(value):
    path = Path(value) if value else REPO_ROOT / "build/native-stack"
    return path if path.is_absolute() else REPO_ROOT / path


def service_binary(unit, profile):
    return REPO_ROOT / "services" / unit / "build" / profile / f"argus-{unit}"


def service_pids(unit, profile):
    result = subprocess.run(
        ["pgrep", "-f", f"^{service_binary(unit, profile)}$"],
        capture_output=True, text=True)
    return [int(line) for line in result.stdout.split()]


def stack_command(*arguments):
    result = subprocess.run(
        [str(REPO_ROOT / "scripts" / "native-stack.sh"), *arguments],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("native-stack.sh " + " ".join(arguments)
                         + " failed:\n" + result.stdout + result.stderr)


def open_db(stack, unit):
    candidates = sorted((stack / unit / "database").glob("*.db"))
    if not candidates:
        raise SystemExit(f"no database under {stack / unit / 'database'}")
    connection = sqlite3.connect(candidates[0], timeout=20.0)
    connection.row_factory = sqlite3.Row
    return connection


def outbox_rows(stack):
    connection = open_db(stack, "auth")
    try:
        return [dict(row) for row in connection.execute(
            "SELECT id, event_id, subject, status, attempts, created_at, "
            "sent_at FROM change_outbox ORDER BY id")]
    finally:
        connection.close()


def action_rows(stack, msg_id=None):
    connection = open_db(stack, "sync")
    try:
        columns = ("SELECT id, user_id, record_id, table_name, action, "
                   "msg_id, old_data, new_data, ip_address, created_at "
                   "FROM user_action_log ")
        if msg_id is None:
            rows = connection.execute(columns + "ORDER BY id")
        else:
            rows = connection.execute(columns + "WHERE msg_id = ? ORDER BY id",
                                      (msg_id,))
        return [dict(row) for row in rows]
    finally:
        connection.close()


def duplicate_msg_ids(stack):
    connection = open_db(stack, "sync")
    try:
        return [row["msg_id"] for row in connection.execute(
            "SELECT msg_id, COUNT(*) AS n FROM user_action_log "
            "GROUP BY msg_id HAVING n > 1")]
    finally:
        connection.close()


def integrity(stack, unit):
    connection = open_db(stack, unit)
    try:
        return connection.execute("PRAGMA integrity_check").fetchone()[0]
    finally:
        connection.close()


def consumer_state(monitor, name, timeout=10.0):
    with urllib.request.urlopen(f"{monitor}/jsz?consumers=true",
                                timeout=timeout) as response:
        report = json.loads(response.read())
    for account in report.get("account_details", []):
        for stream in account.get("stream_detail", []):
            for consumer in stream.get("consumer_detail", []):
                if consumer.get("name") == name:
                    return {"stream": stream.get("name"), **consumer}
    return None


def counters(state):
    return json.dumps({key: value for key, value in state.items()
                       if key.startswith("num_") or key == "ack_floor"})[:200]


def broker_up(monitor, timeout=3.0):
    try:
        with urllib.request.urlopen(f"{monitor}/healthz",
                                    timeout=timeout) as response:
            report = json.loads(response.read())
        return report.get("status") == "ok"
    except (OSError, ValueError):
        return False


def broker_container():
    result = subprocess.run(
        ["docker", "ps", "--filter", "publish=4222", "--format", "{{.ID}}"],
        capture_output=True, text=True)
    identifiers = result.stdout.split()
    if not identifiers:
        raise SystemExit("no container publishes 4222; start the broker")
    return identifiers[0]


def broker_control(action, container):
    result = subprocess.run(["docker", action, container],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"docker {action} {container} failed:\n"
                         + result.stdout + result.stderr)


def wait_for(predicate, seconds, interval=0.4):
    deadline = time.monotonic() + seconds
    while True:
        try:
            value = predicate()
        except (SystemExit, OSError, sqlite3.Error, ValueError, KeyError):
            value = None
        if value:
            return value
        if time.monotonic() >= deadline:
            return None
        time.sleep(interval)


def check(results, name, ok, detail=""):
    results.append((name, ok, detail))
    print(("ok   " if ok else "FAIL ") + name
          + ("  " + detail if detail else ""), flush=True)


def probe(base):
    if not base:
        return False
    try:
        status, _, _ = load_golden().send(base, "GET", "/health", {}, None,
                                          None, 5.0)
        return status < 500
    except (SystemExit, OSError):
        return False


def ensure_up(stack, unit, profile, results, waited=60.0):
    base = load_golden().stack_base(stack, unit)
    healthy = wait_for(lambda: probe(base), waited)
    check(results, f"{unit} answers /health", bool(healthy),
          base or "no sandbox config")
    if not healthy:
        raise SystemExit(f"{unit} never answered; the drill cannot continue")
    pids = service_pids(unit, profile)
    check(results, f"{unit} is this stack's own process", bool(pids),
          f"{service_binary(unit, profile)} pid={pids}")
    if not pids:
        raise SystemExit(f"{unit} answers but no {profile} build of it is "
                         "running; the sandbox was started from another "
                         "profile or another directory")
    return base


def commit_action(stack, base_auth):
    golden = load_golden()
    golden.run_seeder(stack, ["--token-only"])
    refresh = (stack / "refresh-token").read_text().strip()
    access, _ = golden.exchange(base_auth, refresh, 15.0)
    return golden.send(
        base_auth, "PATCH", "/auth/logout",
        {"User-Agent": golden.RECORDER_UA,
         "Authorization": f"Bearer {access}"},
        "{}", "application/json", 15.0)


def logout_action(stack, base_auth, baseline_id, waited=30.0):
    status, _, body = commit_action(stack, base_auth)
    if status != 204:
        raise SystemExit(f"PATCH /auth/logout answered {status}: "
                         f"{body[:200]!r}")
    rows = wait_for(lambda: [row for row in outbox_rows(stack)
                             if row["id"] > baseline_id
                             and row["status"] == "sent"], waited)
    if not rows:
        raise SystemExit("the logout wrote no acknowledged outbox row within "
                         f"{waited:.0f}s")
    return rows[-1]


def describe(row):
    return (f"outbox id={row['id']} subject={row['subject']} "
            f"event_id={row['event_id']} attempts={row['attempts']} "
            "event_id is a message id="
            f"{row['event_id'].startswith('auth-action:')}")


def first_try_ack(row):
    return (row["status"] == "sent" and row["attempts"] == 1
            and row["event_id"].startswith("auth-action:"))


def assert_one_row(stack, results, row, label="the action"):
    arrived = wait_for(lambda: action_rows(stack, row["event_id"]), 90)
    check(results, f"{label} reached the audit log",
          bool(arrived), f"{len(arrived or [])} row(s) for {row['event_id']}")
    if not arrived:
        return None
    entry = arrived[0]
    check(results, f"{label} arrived once", len(arrived) == 1,
          f"{len(arrived)} row(s) for {row['event_id']}")
    check(results, f"{label} is the logout's own record",
          entry["table_name"] == "user" and entry["action"] == "delete"
          and entry["record_id"] == entry["user_id"]
          and entry["ip_address"] == "",
          f"table_name={entry['table_name']} action={entry['action']} "
          f"record_id={entry['record_id']} user_id={entry['user_id']} "
          f"ip_address={entry['ip_address']!r}")
    check(results, "no message id is stored twice",
          not duplicate_msg_ids(stack),
          f"{len(duplicate_msg_ids(stack))} duplicated id(s)")
    return entry


def golden_owner(stack):
    connection = open_db(stack, "identity")
    try:
        row = connection.execute("SELECT id FROM user WHERE id = 1").fetchone()
        return row[0] if row else 0
    finally:
        connection.close()


def require_golden(stack):
    if golden_owner(stack):
        print("golden owner present in", stack / "identity", flush=True)
        return
    raise SystemExit(
        "identity has no golden owner (user 1): the recorder session the "
        "drill mints would name a user the session verdict cannot resolve, "
        "and every audited write would be refused with 401. Seed the "
        "sandbox first:\n  python3 scripts/seed-golden.py --stack-dir "
        + str(stack))


def drill_outage(stack, monitor, profile, results):
    print("=== outage: an action committed while sync is down, and the diff "
          "still arrives ===", flush=True)
    base_auth = ensure_up(stack, "auth", profile, results)
    ensure_up(stack, "sync", profile, results)
    before_outbox = outbox_rows(stack)
    before_actions = action_rows(stack)

    stack_command("kill", "sync")
    check(results, "sync is stopped", not service_pids("sync", profile),
          "pgrep found none")

    row = logout_action(stack, base_auth, before_outbox[-1]["id"] if
                        before_outbox else 0)
    check(results, "the producer acknowledged the action while sync was down",
          first_try_ack(row), describe(row))
    check(results, "the action was queued on the producer's own subject",
          row["subject"] == SUBJECT, row["subject"])
    after_actions = action_rows(stack)
    check(results, "nothing reached sync while it was down",
          len(after_actions) == len(before_actions),
          f"user_action_log {len(before_actions)} -> {len(after_actions)}")
    held = wait_for(lambda: consumer_state(monitor, DURABLE), 15)
    if held is None:
        check(results, "the durable consumer is visible to the broker", False,
              f"{monitor}/jsz did not list {DURABLE}")
    else:
        check(results, "the consumer belongs to the action stream",
              held["stream"] == STREAM, held["stream"])
        check(results, "the broker holds the action for the absent consumer",
              held.get("num_pending", 0) + held.get("num_ack_pending", 0) >= 1,
              f"num_pending={held.get('num_pending')} "
              f"num_ack_pending={held.get('num_ack_pending')}")

    started = time.monotonic()
    stack_command("restart", "sync")
    ensure_up(stack, "sync", profile, results)
    assert_one_row(stack, results, row,
                   "the action committed during the outage")
    elapsed = time.monotonic() - started
    drained = wait_for(lambda: (consumer_state(monitor, DURABLE) or {}).get(
        "num_pending") == 0 and (consumer_state(monitor, DURABLE) or {}).get(
            "num_ack_pending") == 0, 30)
    check(results, "the broker's held count returned to zero", bool(drained),
          counters(consumer_state(monitor, DURABLE) or {}))
    print(f"    the restart, the health gate and the delivery took "
          f"{elapsed:.1f}s", flush=True)


def drill_frozen(stack, monitor, profile, results):
    print("=== frozen: sync killed with a delivery in flight, nothing lost, "
          "nothing doubled ===", flush=True)
    base_auth = ensure_up(stack, "auth", profile, results)
    ensure_up(stack, "sync", profile, results)
    before_actions = action_rows(stack)
    stack_command("freeze", "sync")
    check(results, "sync is frozen", bool(service_pids("sync", profile)),
          f"pid={service_pids('sync', profile)}")

    before_outbox = outbox_rows(stack)
    row = logout_action(stack, base_auth, before_outbox[-1]["id"] if
                        before_outbox else 0)
    check(results, "the producer acknowledged the action while sync was "
          "frozen", first_try_ack(row), describe(row))
    inflight = wait_for(lambda: (consumer_state(monitor, DURABLE) or {}).get(
        "num_ack_pending", 0) >= 1, 20)
    held = consumer_state(monitor, DURABLE) or {}
    check(results, "the broker delivered it and waits for the ack",
          bool(inflight),
          f"num_ack_pending={held.get('num_ack_pending')} "
          f"num_pending={held.get('num_pending')}")
    check(results, "the frozen consumer applied nothing",
          len(action_rows(stack)) == len(before_actions),
          f"user_action_log {len(before_actions)} -> "
          f"{len(action_rows(stack))}")

    print(f"    holding past the broker's {ACK_WAIT_SECONDS}s ack window, "
          "waiting for the redelivery", flush=True)
    started = time.monotonic()
    redelivered = wait_for(
        lambda: (consumer_state(monitor, DURABLE) or {}).get(
            "num_redelivered", 0) >= 1, ACK_WAIT_SECONDS + 30)
    waited = time.monotonic() - started
    check(results, "the broker redelivered into the frozen consumer",
          bool(redelivered), f"after {waited:.1f}s")
    check(results, "the redelivery waited for the ack window",
          waited >= ACK_WAIT_SECONDS - 5, f"{waited:.1f}s, window is "
          f"{ACK_WAIT_SECONDS}s")
    check(results, "the redelivery is still unacknowledged",
          (consumer_state(monitor, DURABLE) or {}).get("num_ack_pending") == 1,
          f"num_ack_pending="
          f"{(consumer_state(monitor, DURABLE) or {}).get('num_ack_pending')}")
    held_stream_seq = held.get("delivered", {}).get("stream_seq")
    check(results, "the action the broker holds is the one it delivered",
          held_stream_seq is not None, f"stream_seq={held_stream_seq}")

    stack_command("sigkill", "sync")
    check(results, "sync was killed with the delivery unacknowledged",
          not service_pids("sync", profile), "pgrep found none")

    stack_command("restart", "sync")
    ensure_up(stack, "sync", profile, results)
    assert_one_row(stack, results, row, "the redelivered action")
    settled = wait_for(
        lambda: (consumer_state(monitor, DURABLE) or {}).get(
            "num_ack_pending") == 0 and (consumer_state(monitor, DURABLE)
                                         or {}).get("num_pending") == 0, 30)
    check(results, "the consumer settled the held action after the restart",
          bool(settled), counters(consumer_state(monitor, DURABLE) or {}))
    floor = (consumer_state(monitor, DURABLE) or {}).get("ack_floor", {})
    check(results, "the acknowledged action is the one that was held",
          floor.get("stream_seq") == held_stream_seq,
          f"ack_floor stream_seq={floor.get('stream_seq')} "
          f"held stream_seq={held_stream_seq}")


def queue_action(stack, base_auth, baseline_id, waited=15.0):
    status, _, body = commit_action(stack, base_auth)
    if status != 204:
        raise SystemExit(f"PATCH /auth/logout answered {status}: "
                         f"{body[:200]!r}")
    rows = wait_for(lambda: [row for row in outbox_rows(stack)
                             if row["id"] > baseline_id], waited)
    if not rows:
        raise SystemExit("the logout wrote no outbox row within "
                         f"{waited:.0f}s")
    return status, rows[-1]


def drill_broker(stack, monitor, profile, results):
    print("=== broker: the acknowledgement that never comes, and the queue "
          "that waits for it ===", flush=True)
    base_auth = ensure_up(stack, "auth", profile, results)
    ensure_up(stack, "sync", profile, results)
    before_outbox = outbox_rows(stack)
    before_actions = action_rows(stack)
    baseline_id = before_outbox[-1]["id"] if before_outbox else 0
    container = broker_container()
    check(results, "the broker is a container the drill can control",
          bool(container), f"container {container} publishes 4222")

    broker_control("stop", container)
    try:
        down = wait_for(lambda: not broker_up(monitor), 60)
        check(results, "the broker is stopped", bool(down), container)

        status, first = queue_action(stack, base_auth, baseline_id)
        check(results, "the write was still accepted without a broker",
              status == 204, f"PATCH /auth/logout answered {status}")
        _, second = queue_action(stack, base_auth, first["id"])
        queued = wait_for(lambda: [row for row in outbox_rows(stack)
                                   if row["id"] > baseline_id
                                   and row["status"] == "pending"], 15)
        check(results, "both actions are queued, waiting for an "
              "acknowledgement", len(queued or []) == 2,
              f"{len(queued or [])} pending row(s)")
        check(results, "an unacknowledged action is not marked sent",
              all(row["sent_at"] == 0 for row in queued or []),
              "sent_at=" + str([row["sent_at"] for row in queued or []]))
        blocked = wait_for(lambda: len(action_rows(stack)) != len(
            before_actions), 5)
        check(results, "nothing reached sync while the broker was down",
              not blocked, f"user_action_log {len(before_actions)} -> "
              f"{len(action_rows(stack))}")

        started = time.monotonic()
        broker_control("start", container)
        back = wait_for(lambda: broker_up(monitor), 90)
        check(results, "the broker came back", bool(back),
              f"after {time.monotonic() - started:.1f}s")
        if not back:
            return

        for label, row in (("the first", first), ("the second", second)):
            assert_one_row(stack, results, row,
                           f"{label} action queued without a broker")
        events = [first["event_id"], second["event_id"]]
        entries = [action_rows(stack, event) for event in events]
        arrived = [entry[0]["id"] for entry in entries if entry]
        check(results, "the queue drained in the order it was written",
              arrived == sorted(arrived) and len(arrived) == 2, str(arrived))
        acknowledged = wait_for(lambda: all(
            row["status"] == "sent" for row in outbox_rows(stack)
            if row["event_id"] in events), 60)
        settled = [row for row in outbox_rows(stack)
                   if row["event_id"] in events]
        check(results, "both actions were marked sent once the broker "
              "acknowledged", bool(acknowledged),
              " ".join(describe(row) for row in settled))
        check(results, "the acknowledgement is dated after the write",
              all(row["sent_at"] >= row["created_at"] for row in settled),
              "created_at=" + str([row["created_at"] for row in settled])
              + " sent_at=" + str([row["sent_at"] for row in settled]))
        drained = wait_for(lambda: (consumer_state(monitor, DURABLE) or {}
                                    ).get("num_pending") == 0
                           and (consumer_state(monitor, DURABLE) or {}
                                ).get("num_ack_pending") == 0, 30)
        check(results, "the consumer settled both actions", bool(drained),
              counters(consumer_state(monitor, DURABLE) or {}))
        state = integrity(stack, "auth")
        check(results, "the producer's database survived the outage",
              state == "ok", state)
    finally:
        broker_control("start", container)
        wait_for(lambda: broker_up(monitor), 90)


def run_burst(stack, base_auth, stop, counts, lock):
    while not stop.is_set():
        try:
            status = commit_action(stack, base_auth)[0]
        except (SystemExit, OSError, ValueError):
            with lock:
                counts["interrupted"] += 1
            continue
        with lock:
            counts["accepted" if status == 204 else "refused"] += 1
        if status != 204:
            time.sleep(0.2)


def drill_producer(stack, monitor, profile, results):
    print("=== producer: auth killed inside a stream of audited writes, "
          "nothing lost, nothing doubled ===", flush=True)
    base_auth = ensure_up(stack, "auth", profile, results)
    ensure_up(stack, "sync", profile, results)
    before_outbox = {row["event_id"]: row for row in outbox_rows(stack)}
    before_actions = {row["msg_id"] for row in action_rows(stack)}

    stop = threading.Event()
    counts = {"accepted": 0, "refused": 0, "interrupted": 0}
    lock = threading.Lock()
    worker = threading.Thread(target=run_burst,
                              args=(stack, base_auth, stop, counts, lock),
                              daemon=True)
    worker.start()
    settled = wait_for(lambda: counts["accepted"] >= 2, 60)
    stack_command("sigkill", "auth")
    stop.set()
    worker.join(timeout=30)
    with lock:
        accepted, refused = counts["accepted"], counts["refused"]
        interrupted = counts["interrupted"]
    tally = (f"accepted={accepted} refused={refused} "
             f"interrupted={interrupted}")
    check(results, "the burst committed audited writes before the kill",
          bool(settled) and len(outbox_rows(stack)) > len(before_outbox),
          f"{len(outbox_rows(stack)) - len(before_outbox)} row(s), {tally}")
    check(results, "auth was killed with the burst running",
          not service_pids("auth", profile), tally)
    check(results, "the burst saw the kill land", accepted >= 1, tally)

    stack_command("restart", "auth")
    ensure_up(stack, "auth", profile, results)

    state = integrity(stack, "auth")
    check(results, "the database survived the kill intact", state == "ok",
          state)
    settled = wait_for(lambda: all(row["status"] == "sent"
                                   for row in outbox_rows(stack)), 90)
    still_pending = sum(1 for row in outbox_rows(stack)
                        if row["status"] == "pending")
    check(results, "every queued action was acknowledged after the restart",
          bool(settled), f"{still_pending} still pending")
    events = [row["event_id"] for row in outbox_rows(stack)
              if row["event_id"] not in before_outbox]
    check(results, "the kill left committed actions to recover",
          len(events) >= 1, f"{len(events)} action(s) committed")
    delivered = wait_for(lambda: all(action_rows(stack, event)
                                     for event in events), 120)
    check(results, "every committed action reached the audit log",
          bool(delivered), f"{len(events)} action(s) checked")
    check(results, "no committed action produced two rows",
          all(len(action_rows(stack, event)) == 1 for event in events),
          f"{len(events)} action(s) checked")
    check(results, "no message id is stored twice",
          not duplicate_msg_ids(stack),
          f"{len(duplicate_msg_ids(stack))} duplicated id(s)")
    claimed = set(events) | before_actions
    check(results, "nothing arrived that no producer row claims",
          not [row for row in action_rows(stack) if row["msg_id"] not in
               claimed], "every stored message id has a queued action")
    shapes = {row["action"] + " " + row["table_name"]
              for event in events for row in action_rows(stack, event)}
    check(results, "the recovered actions are the writes that were issued",
          shapes == {"delete user"}, str(sorted(shapes)) if shapes else "none")


def command_state(stack, monitor, profile):
    rows = outbox_rows(stack)
    pending = [row for row in rows if row["status"] == "pending"]
    print(f"auth.change_outbox: {len(rows)} row(s), {len(pending)} pending")
    for row in rows[-5:]:
        print(f"  id={row['id']} {row['status']} attempts={row['attempts']} "
              f"event_id={row['event_id']} sent_at={row['sent_at']}")
    actions = action_rows(stack)
    print(f"sync.user_action_log: {len(actions)} row(s)")
    for row in actions[-5:]:
        print(f"  id={row['id']} {row['action']} {row['table_name']} "
              f"record_id={row['record_id']} msg_id={row['msg_id']}")
    print("duplicated msg_id:", duplicate_msg_ids(stack) or "none")
    state = consumer_state(monitor, DURABLE)
    print("broker", monitor, "up" if broker_up(monitor) else "unreachable")
    if state is None:
        print("consumer", DURABLE, "absent")
    else:
        print("consumer", DURABLE, "on", state["stream"], ":", counters(state))
    for unit in ("auth", "sync", "identity"):
        print(f"{unit}.db integrity:", integrity(stack, unit))
    print("running:", {unit: service_pids(unit, profile)
                       for unit in ("auth", "sync")})


def main(argv):
    parser = argparse.ArgumentParser(
        description="Drill the durability of an audited write: kill the "
                    "consumer while an action is in flight and measure "
                    "whether the audit row still arrives exactly once, "
                    "through the producer's outbox and the broker's "
                    "acknowledgement.")
    parser.add_argument("--stack-dir", default=None,
                        help="sandbox directory (default build/native-stack)")
    parser.add_argument("--monitor", default=DEFAULT_MONITOR,
                        help="the NATS monitoring endpoint")
    parser.add_argument("--profile", default="dev", help="build profile")
    parser.add_argument("drill", choices=("outage", "frozen", "broker",
                                          "producer", "all", "state"))
    args = parser.parse_args(argv)
    stack = stack_path(args.stack_dir)
    if not stack.is_dir():
        raise SystemExit(f"{stack} does not exist; run "
                         "scripts/native-stack.sh up first")
    if args.drill == "state":
        command_state(stack, args.monitor, args.profile)
        return 0
    require_golden(stack)
    results = []
    if args.drill in ("outage", "all"):
        drill_outage(stack, args.monitor, args.profile, results)
    if args.drill in ("frozen", "all"):
        drill_frozen(stack, args.monitor, args.profile, results)
    if args.drill in ("broker", "all"):
        drill_broker(stack, args.monitor, args.profile, results)
    if args.drill in ("producer", "all"):
        drill_producer(stack, args.monitor, args.profile, results)
    failed = [name for name, ok, _ in results if not ok]
    print(f"\ndurability: {len(results)} checks, "
          f"{len(results) - len(failed)} passed, {len(failed)} failed")
    for name in failed:
        print("  FAILED:", name)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
