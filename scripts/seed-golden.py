"""Seed the golden /sync verify state into a scratch database copy.

Inserts the fixture-shaped rows the golden-sync test fixtures expect:
the camera/zone rows (Golden Cam / Golden Zone) plus the calendar_event
with ends_at NULL and the project_task with assignee_id 1, and the
identity rows (Golden user/person, one zone audit row, one reminder
user-audit row) with a fresh recorder refresh session minted from the
gateway config's JWT secrets. Secrets are read from the config file and
never printed; the rotated refresh token is written 0600 for
ARGUS_TEST_REFRESH_TOKEN.

Run it against a COPY of the real databases, with the stack stopped:
camera-init migrates the camera/zone rows onto the camera.db volume
before the first boot (after that, camera.db is live data and the tool
correctly no-ops).

Usage: seed-golden.py --argus argus.db --identity identity.db \
         --config argus-deploy/config.gateway.toml [--device-ip 127.0.0.1]
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import sqlite3
import time
import tomllib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--argus", required=True)
parser.add_argument("--identity", required=True)
parser.add_argument("--config", required=True)
parser.add_argument("--device-ip", default="127.0.0.1")
parser.add_argument("--token-out", default="/tmp/argus-golden-refresh-token")
args = parser.parse_args()

with open(args.config, "rb") as fh:
    cfg = tomllib.load(fh)
jwt_secret = cfg["jwt"]["secret"]
refresh_secret = cfg["jwt"]["refresh_secret"]
fp_secret = cfg["device"]["fingerprint_secret"]

RECORDER_UA = "argus-golden-recorder/1.0"
now = int(time.time())


def b64u(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def mint(claims: dict, secret: str) -> str:
    header = b64u(json.dumps({"alg": "HS256", "typ": "JWT"},
                             separators=(",", ":")).encode())
    payload = b64u(json.dumps(claims, separators=(",", ":")).encode())
    signing = f"{header}.{payload}".encode()
    sig = b64u(hmac.new(secret.encode(), signing, hashlib.sha256).digest())
    return f"{header}.{payload}.{sig}"


con = sqlite3.connect(args.argus)
try:
    cur = con.cursor()
    if cur.execute("SELECT COUNT(*) FROM camera WHERE name='Golden Cam'"
                   ).fetchone()[0] == 0:
        # Fixture-shape rows: calendar_event.ends_at stays NULL and
        # project_task.assignee_id is 1 — the normalized fixtures mask
        # exactly these and nothing else.
        cur.execute(
            "INSERT INTO calendar_event(created_by,owner_id,project_id,"
            "title,description,location,color,starts_at,ends_at,is_all_day,"
            "recurrence_rule,created_at) VALUES(1,1,NULL,'Golden event',"
            "'Fixture row','','',?,NULL,0,NULL,?)", (now, now))
        cur.execute(
            "INSERT INTO notification(user_id,type,title,body,data,is_read,"
            "created_at) VALUES(1,'system','Golden notification','Fixture"
            " row','{}',0,?)", (now,))
        cur.execute(
            "INSERT INTO project(owner_id,name,description,status,color,"
            "starts_at,target_at,created_at) VALUES(1,'Golden project',"
            "'Fixture row','active','',NULL,NULL,?)", (now,))
        cur.execute(
            "INSERT INTO project_member(project_id,user_id,access,created_at)"
            " VALUES(1,1,'edit',?)", (now,))
        cur.execute(
            "INSERT INTO project_task(project_id,created_by,assignee_id,"
            "title,status,priority,due_at,sort_order,created_at)"
            " VALUES(1,1,1,'Golden task','todo','low',NULL,0,?)", (now,))
        cur.execute(
            "INSERT INTO reminder(created_by,target_user_id,title,"
            "description,scheduled_at,recurrence_rule,is_completed,"
            "created_at) VALUES(1,1,'Golden reminder','Fixture row',?,NULL,"
            "0,?)", (now, now))
        cur.execute(
            "INSERT INTO camera(name,manufacturer,model,ip,port,username,"
            "password,cloud_username,cloud_password,driver,icon,"
            "record_mode,retention_days,capabilities,config,is_enabled,"
            "is_online,created_at) VALUES('Golden Cam','tapo','C200',"
            "'127.0.0.1',1,'admin','','','','tapo','video','events',7,"
            "'[]','{}',1,0,?)", (now,))
        cam_id = cur.lastrowid
        cur.execute(
            "INSERT INTO zone(camera_id,name,points,zone_type,color,"
            "is_enabled,created_at) VALUES(?,'Golden Zone','[]','monitor',"
            "'#00FF00',1,?)", (cam_id, now))
        con.commit()
        print("argus.db golden rows seeded")
    else:
        print("argus.db golden rows already present (skipped)")
    for t in ("camera", "zone"):
        print("argus.db", t,
              cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0])
finally:
    con.close()

con = sqlite3.connect(args.identity)
try:
    cur = con.cursor()
    if cur.execute("SELECT COUNT(*) FROM user WHERE id=1").fetchone()[0] == 0:
        cur.execute(
            "INSERT INTO user(id,name,last_name,role,lang,is_active,"
            "created_at) VALUES(1,'Golden','Recorder','owner','en',1,?)",
            (now,))
        cur.execute(
            "INSERT INTO person(user_id,name,alias,observation,"
            "first_seen_at,last_seen_at,created_at)"
            " VALUES(1,'Golden','Rec','',?,?,?)", (now, now, now))
        # Golden audit rows exactly as the sync fixtures expect (priority 1).
        cur.execute(
            "INSERT INTO audit_log(create_user_id,record_id,table_name,"
            "changes,priority,event_timestamp,created_at)"
            " VALUES(1,1,'zone','{\"name\":{\"current\":\"Golden Zone\","
            "\"previous\":\"Old Zone\"}}',1,?,?)", (now, now))
        cur.execute(
            "INSERT INTO user_audit_log(user_id,record_id,table_name,"
            "changes,priority,event_timestamp,created_at)"
            " VALUES(1,1,'reminder','{\"title\":{\"current\":\"Golden"
            " reminder\",\"previous\":\"Old reminder\"}}',1,?,?)", (now, now))
        print("identity.db golden rows seeded")
    else:
        print("identity.db golden rows already present (skipped)")
    device_hash = hmac.new(fp_secret.encode(),
                           f"{RECORDER_UA}|{args.device_ip}".encode(),
                           hashlib.sha256).hexdigest()
    refresh = mint({"iss": "argus", "sub": "1", "iat": now,
                    "exp": now + 864000}, refresh_secret)
    cur.execute(
        "INSERT INTO refresh_token(user_id,access_token,refresh_token,"
        "device_hash,user_agent,is_valid,is_used,expires_at,created_at)"
        " VALUES(1,?,?,?,?,1,0,?,?)",
        (mint({"iss": "argus", "sub": "1", "iat": now, "exp": now + 900},
              jwt_secret), refresh, device_hash, RECORDER_UA, now + 86400,
         now))
    con.commit()
    for t in ("user", "person", "audit_log", "user_audit_log",
              "refresh_token"):
        print("identity.db", t,
              cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0])
finally:
    con.close()

with open(args.token_out, "w") as fh:
    fh.write(refresh + "\n")
os.chmod(args.token_out, 0o600)
print("recorder refresh token written (0600):", args.token_out,
      "- consume once, then read the rotated one from identity.db")
