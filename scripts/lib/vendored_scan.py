#!/usr/bin/env python3

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path


SHA256 = re.compile(r"[0-9a-f]{64}")
ESCAPES = {"a": 7, "b": 8, "t": 9, "n": 10, "v": 11, "f": 12, "r": 13,
           '"': 34, "\\": 92}
OCTAL = "01234567"


def patch_directories(root):
    base = root / "third_party"
    if not base.is_dir():
        return []
    return sorted(path for path in base.glob("*/argus-patches") if path.is_dir())


def in_work_tree(root):
    try:
        done = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--is-inside-work-tree"],
            capture_output=True, text=True)
    except OSError:
        return False
    return done.returncode == 0 and done.stdout.strip() == "true"


def unquote_path(field):
    if not (field.startswith('"') and field.endswith('"') and len(field) >= 2):
        return field
    out = bytearray()
    index, last = 1, len(field) - 1
    while index < last:
        char = field[index]
        if char != "\\":
            out.extend(char.encode("utf-8"))
            index += 1
            continue
        index += 1
        if index >= last:
            return None
        escape = field[index]
        if escape in ESCAPES:
            out.append(ESCAPES[escape])
            index += 1
        elif escape in OCTAL:
            digits = field[index:index + 3]
            if len(digits) != 3 or any(digit not in OCTAL for digit in digits):
                return None
            value = int(digits, 8)
            if value > 255:
                return None
            out.append(value)
            index += 3
        else:
            return None
    try:
        return out.decode("utf-8")
    except UnicodeDecodeError:
        return None


def guarded_files(patch):
    marker = "+++ "
    touched, unreadable = [], []
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith(marker):
            continue
        field = line[len(marker):].strip()
        rel = unquote_path(field)
        if rel is None or not rel.startswith("b/"):
            unreadable.append(field)
            continue
        rel = rel[2:]
        if rel and rel != "/dev/null" and rel not in touched:
            touched.append(rel)
    return touched, unreadable


def pin_entries(pin, root):
    entries, problems = [], []
    text = pin.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        digest = parts[0].lower()
        if len(parts) != 2 or not SHA256.fullmatch(digest):
            problems.append(
                f"pin-unreadable: {pin.relative_to(root)}:{number} is not a "
                f"sha256sum line -- write it with: sha256sum <file> > "
                f"{pin.relative_to(root)}")
            continue
        rel = parts[1].strip()
        if rel.startswith("*"):
            rel = rel[1:].strip()
        entries.append((digest, rel))
    return entries, problems


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(
        prog="check-vendored",
        description="The recorded patch and pin state of the third_party "
                    "trees that carry an argus-patches directory.")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    args = parser.parse_args()
    root = Path(args.root).resolve()

    directories = patch_directories(root)
    if not directories:
        print(f"check-vendored: no third_party/*/argus-patches under {root} "
              f"-- nothing was checked", file=sys.stderr)
        return 2

    patches = sorted(patch for directory in directories
                     for patch in directory.glob("*.patch"))
    pins = sorted(pin for directory in directories
                  for pin in directory.glob("*.sha256"))
    if not patches and not pins:
        print(f"check-vendored: {len(directories)} argus-patches "
              f"director{'y' if len(directories) == 1 else 'ies'} under "
              f"{root} hold no .patch and no .sha256 -- nothing was checked",
              file=sys.stderr)
        return 2

    failures = []
    pinned = {}
    checked = 0
    for pin in pins:
        entries, problems = pin_entries(pin, root)
        failures.extend(problems)
        for digest, rel in entries:
            pinned.setdefault(pin.parent, set()).add(rel)
            target = root / rel
            if not target.is_file():
                failures.append(
                    f"pin-missing-file: {rel}, named by "
                    f"{pin.relative_to(root)}, does not exist -- update the "
                    f"pin or restore the file")
                continue
            actual = sha256_of(target)
            checked += 1
            if actual != digest:
                failures.append(
                    f"pin-mismatch: {rel} is {actual}, pinned {digest} in "
                    f"{pin.relative_to(root)} -- the vendored file moved; if "
                    f"that was deliberate, update the pin with: sha256sum "
                    f"{rel} > {pin.relative_to(root)}")

    for patch in patches:
        touched, unreadable = guarded_files(patch)
        for field in unreadable:
            failures.append(
                f"unreadable-path: {patch.relative_to(root)} names {field} on "
                f"a +++ line, which this gate cannot read as a decoded b/ "
                f"path -- it cannot tell which file is guarded; use a plain "
                f"path or record the pin by hand")
        for rel in touched:
            if rel not in pinned.get(patch.parent, set()):
                failures.append(
                    f"unpinned: {patch.relative_to(root)} touches {rel} and "
                    f"no pin in {patch.parent.relative_to(root)} records it "
                    f"-- add {Path(rel).name}.sha256 with: sha256sum {rel} > "
                    f"{(patch.parent / (Path(rel).name + '.sha256')).relative_to(root)}")

    skipped = not in_work_tree(root)
    if not skipped:
        for patch in patches:
            done = subprocess.run(
                ["git", "-C", str(root), "apply", "--reverse", "--check",
                 "--", str(patch.relative_to(root))],
                capture_output=True, text=True)
            if done.returncode != 0:
                detail = (done.stderr or done.stdout).strip().splitlines()
                first = detail[0] if detail else "no output"
                failures.append(
                    f"unapplied: {patch.relative_to(root)} does not apply in "
                    f"reverse ({first}) -- the vendored tree does not carry "
                    f"it; re-apply it with: git apply "
                    f"{patch.relative_to(root)}")

    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"check-vendored: scope: "
          f"{', '.join(str(directory.relative_to(root)) for directory in directories)}"
          f" (a third_party tree without an argus-patches directory is not "
          f"covered)")
    if skipped:
        print(f"check-vendored: {root} is not a git work tree, so the "
              f"reverse-apply leg of the {len(patches)} patch(es) was "
              f"skipped; the {len(pins)} pin file(s) were still checked")
    print(f"check-vendored: {len(patches)} patch(es), {len(pins)} pin "
          f"file(s), {checked} pinned file(s) checked, "
          f"{len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
