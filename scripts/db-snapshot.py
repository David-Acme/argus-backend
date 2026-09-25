#!/usr/bin/env python3
import argparse
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DIGEST_LENGTH = 16


def table_rows(connection, name):
    quoted = '"' + name.replace('"', '""') + '"'
    try:
        return connection.execute(f"SELECT * FROM {quoted} ORDER BY rowid"
                                  ).fetchall()
    except sqlite3.Error:
        return connection.execute(f"SELECT * FROM {quoted}").fetchall()


def snapshot(stack):
    result = {}
    for path in sorted(stack.glob("*/database/*.db")):
        key = f"{path.parent.parent.name}/{path.name}"
        tables = {}
        connection = sqlite3.connect(path, timeout=5.0)
        try:
            names = [row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table' "
                "ORDER BY name")]
            for name in names:
                try:
                    rows = table_rows(connection, name)
                    digest = hashlib.sha256(
                        repr(rows).encode("utf-8")).hexdigest()[:DIGEST_LENGTH]
                    tables[name] = {"rows": len(rows), "digest": digest}
                except sqlite3.Error as error:
                    tables[name] = {"rows": None, "digest": None,
                                    "error": str(error)}
        finally:
            connection.close()
        result[key] = tables
    return result


def load(path):
    return json.loads(Path(path).read_text())


def describe(diff):
    if not diff:
        return "unchanged"
    return ", ".join(diff)


def compare(before, after):
    changed = 0
    for database in sorted(set(before) | set(after)):
        old_tables = before.get(database, {})
        new_tables = after.get(database, {})
        for table in sorted(set(old_tables) | set(new_tables)):
            old = old_tables.get(table)
            new = new_tables.get(table)
            if old == new:
                continue
            changed += 1
            if old is None:
                print(f"{database}.{table}: created "
                      f"({new.get('rows')} rows)")
                continue
            if new is None:
                print(f"{database}.{table}: dropped "
                      f"({old.get('rows')} rows)")
                continue
            diff = []
            if old.get("rows") != new.get("rows"):
                diff.append(f"{old.get('rows')} -> {new.get('rows')} rows")
            if old.get("digest") != new.get("digest"):
                diff.append("content changed")
            if old.get("error") != new.get("error"):
                diff.append(f"error {old.get('error')!r} -> {new.get('error')!r}")
            print(f"{database}.{table}: {describe(diff)}")
    if not changed:
        print("every table of every database is byte-identical")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Snapshot the row count and content digest of every table "
                    "of every database under a sandbox stack, and compare two "
                    "snapshots, so a drill can state exactly what it wrote.")
    parser.add_argument("--stack-dir",
                        default=str(REPO_ROOT / "build/native-stack"))
    parser.add_argument("--out", help="write the snapshot here; without it the "
                                      "snapshot goes to stdout")
    parser.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"),
                        help="compare two snapshots instead of taking one")
    args = parser.parse_args(argv)

    if args.diff:
        return compare(load(args.diff[0]), load(args.diff[1]))

    stack = Path(args.stack_dir)
    if not stack.is_dir():
        print(f"{stack} does not exist; run scripts/native-stack.sh up",
              file=sys.stderr)
        return 1
    data = snapshot(stack)
    text = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if args.out:
        Path(args.out).write_text(text)
        tables = sum(len(value) for value in data.values())
        print(f"snapshot: {len(data)} databases, {tables} tables -> {args.out}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
