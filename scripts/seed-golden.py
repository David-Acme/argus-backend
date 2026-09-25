#!/usr/bin/env python3
import argparse
import base64
import hashlib
import hmac
import json
import os
import sqlite3
import sys
import time
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RECORDER_UA = "argus-golden-recorder/1.0"

SERVICE_DB = {
    "auth": "auth",
    "identity": "identity",
    "camera": "camera",
    "productivity": "productivity",
    "notification": "notifications",
    "sync": "sync",
}


def b64u(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def mint(claims: dict, secret: str) -> str:
    header = b64u(json.dumps({"alg": "HS256", "typ": "JWT"},
                             separators=(",", ":")).encode())
    payload = b64u(json.dumps(claims, separators=(",", ":")).encode())
    signing = f"{header}.{payload}".encode()
    signature = b64u(hmac.new(secret.encode(), signing, hashlib.sha256).digest())
    return f"{header}.{payload}.{signature}"


def connect(stack: Path, service: str) -> tuple[sqlite3.Connection, dict]:
    config_path = stack / service / "config.toml"
    if not config_path.is_file():
        raise SystemExit(f"missing {config_path}; run scripts/native-stack.sh up")
    with open(config_path, "rb") as handle:
        config = tomllib.load(handle)
    section = config[SERVICE_DB[service]]
    db_path = Path(section["db"])
    if not db_path.is_absolute():
        db_path = config_path.parent / db_path
    db_path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(db_path)
    connection.executescript(Path(section["schema"]).read_text())
    return connection, config


def apply_schemas(stack: Path) -> None:
    for service in SERVICE_DB:
        connection, _ = connect(stack, service)
        connection.commit()
        connection.close()
    print("schemas applied under", stack)


def mint_session(auth: sqlite3.Connection, auth_config: dict, now: int,
                 device_ip: str, token_out: Path) -> None:
    device_hash = hmac.new(
        auth_config["device"]["fingerprint_secret"].encode(),
        f"{RECORDER_UA}|{device_ip}".encode(),
        hashlib.sha256).hexdigest()
    access = mint({"iss": "argus", "sub": "1", "iat": now, "exp": now + 900},
                  auth_config["jwt"]["secret"])
    refresh = mint({"iss": "argus", "sub": "1", "iat": now, "exp": now + 864000},
                   auth_config["jwt"]["refresh_secret"])
    auth.execute("DELETE FROM refresh_token")
    auth.execute(
        "INSERT INTO refresh_token(user_id,access_token,refresh_token,"
        "device_hash,user_agent,is_valid,is_used,expires_at,created_at)"
        " VALUES(1,?,?,?,?,1,0,?,?)",
        (access, refresh, device_hash, RECORDER_UA, now + 86400, now))
    auth.commit()
    token_out.parent.mkdir(parents=True, exist_ok=True)
    token_out.write_text(refresh + "\n")
    os.chmod(token_out, 0o600)
    print("recorder refresh token written (0600):", token_out)


def seed(stack: Path, device_ip: str, token_out: Path) -> None:
    now = int(time.time())
    auth, auth_config = connect(stack, "auth")
    identity, _ = connect(stack, "identity")
    camera, _ = connect(stack, "camera")
    productivity, _ = connect(stack, "productivity")
    notification, _ = connect(stack, "notification")
    sync, _ = connect(stack, "sync")

    identity.execute(
        "INSERT INTO user(id,name,last_name,role,lang,is_active,created_at)"
        " VALUES(1,'Golden','Recorder','owner','en',1,?)", (now,))
    identity.execute(
        "INSERT INTO person(user_id,name,alias,observation,status,"
        "first_seen_at,last_seen_at,created_at)"
        " VALUES(1,'Golden','Rec','','known',?,?,?)", (now, now, now))

    camera.execute(
        "INSERT INTO camera(name,manufacturer,model,ip,port,username,password,"
        "cloud_username,cloud_password,driver,icon,record_mode,retention_days,"
        "capabilities,config,is_enabled,is_online,created_at)"
        " VALUES('Golden Cam','tapo','C200','127.0.0.1',1,'admin','','','',"
        "'tapo','video','events',7,'[]','{}',1,0,?)", (now,))
    camera_id = camera.execute("SELECT last_insert_rowid()").fetchone()[0]
    camera.execute(
        "INSERT INTO zone(camera_id,name,points,zone_type,color,is_enabled,"
        "created_at) VALUES(?,'Golden Zone','[]','monitor','#00FF00',1,?)",
        (camera_id, now))
    zone_id = camera.execute("SELECT last_insert_rowid()").fetchone()[0]

    productivity.execute(
        "INSERT INTO calendar_event(created_by,owner_id,project_id,title,"
        "description,location,color,starts_at,ends_at,is_all_day,"
        "recurrence_rule,created_at)"
        " VALUES(1,1,NULL,'Golden event','Fixture row','','',?,NULL,0,NULL,?)",
        (now, now))
    productivity.execute(
        "INSERT INTO project(id,owner_id,name,description,status,color,"
        "starts_at,target_at,created_at)"
        " VALUES(1,1,'Golden project','Fixture row','active','',NULL,NULL,?)",
        (now,))
    productivity.execute(
        "INSERT INTO project_member(project_id,user_id,access,created_at)"
        " VALUES(1,1,'edit',?)", (now,))
    productivity.execute(
        "INSERT INTO project_task(project_id,created_by,assignee_id,title,"
        "status,priority,due_at,sort_order,created_at)"
        " VALUES(1,1,1,'Golden task','todo','low',NULL,0,?)", (now,))
    productivity.execute(
        "INSERT INTO reminder(created_by,target_user_id,title,description,"
        "scheduled_at,recurrence_rule,is_completed,created_at)"
        " VALUES(1,1,'Golden reminder','Fixture row',?,NULL,0,?)", (now, now))
    reminder_id = productivity.execute(
        "SELECT last_insert_rowid()").fetchone()[0]

    notification.execute(
        "INSERT INTO notification(user_id,type,title,body,data,is_read,"
        "created_at) VALUES(1,'system','Golden notification','Fixture row',"
        "'{}',0,?)", (now,))

    sync.execute(
        "INSERT INTO audit_log(create_user_id,record_id,table_name,changes,"
        "priority,event_timestamp,created_at)"
        " VALUES(1,?,'zone',?,1,?,?)",
        (zone_id,
         json.dumps({"name": {"current": "Golden Zone",
                              "previous": "Old Zone"}}), now, now))
    sync.execute(
        "INSERT INTO user_audit_log(user_id,record_id,table_name,changes,"
        "priority,event_timestamp,created_at)"
        " VALUES(1,?,'reminder',?,1,?,?)",
        (reminder_id,
         json.dumps({"title": {"current": "Golden reminder",
                               "previous": "Old reminder"}}), now, now))

    mint_session(auth, auth_config, now, device_ip, token_out)

    counts = {}
    for service, connection, tables in (
            ("identity", identity, ("user", "person", "user_invitation")),
            ("camera", camera, ("camera", "zone", "camera_stream")),
            ("productivity", productivity,
             ("calendar_event", "calendar_event_share", "project",
              "project_member", "project_task", "reminder",
              "reminder_detail")),
            ("notification", notification, ("notification",)),
            ("sync", sync, ("audit_log", "user_audit_log")),
            ("auth", auth, ("refresh_token",)),
    ):
        connection.commit()
        for table in tables:
            try:
                counts[f"{service}.{table}"] = connection.execute(
                    f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            except sqlite3.OperationalError:
                counts[f"{service}.{table}"] = "no table"

    for name, value in counts.items():
        print(f"  {name:<36} {value}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Seed the native stack with the golden fixture rows.")
    parser.add_argument("--stack-dir",
                        default=str(REPO_ROOT / "build/native-stack"),
                        help="sandbox directory (default build/native-stack)")
    parser.add_argument("--device-ip", default="127.0.0.1",
                        help="address the recorder session is bound to")
    parser.add_argument("--token-out", default=None,
                        help="where to write the refresh token (0600)")
    parser.add_argument("--schemas-only", action="store_true",
                        help="apply every owner's schema and stop")
    parser.add_argument("--token-only", action="store_true",
                        help="mint a fresh recorder session without reseeding")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    stack = Path(args.stack_dir)
    token_out = Path(args.token_out) if args.token_out \
        else stack / "refresh-token"

    if args.schemas_only:
        apply_schemas(stack)
    elif args.token_only:
        auth, auth_config = connect(stack, "auth")
        mint_session(auth, auth_config, int(time.time()), args.device_ip,
                     token_out)
        auth.close()
    else:
        seed(stack, args.device_ip, token_out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
