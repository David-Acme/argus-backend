#!/usr/bin/env python3
import pathlib
import sys
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent
MARKERS = HERE.parent / "fixtures" / "eval" / "peruvian-markers.txt"


def fold(text):
    stripped = "".join(c for c in unicodedata.normalize("NFD", text.lower()) if unicodedata.category(c) != "Mn")
    return " ".join(stripped.split())


def load():
    rows = []
    for line in MARKERS.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            rows.append(line)
    return rows


def hits(reply, table):
    words = fold(reply).split()
    for marker in table:
        needle = fold(marker).split()
        for at in range(len(words) - len(needle) + 1):
            if words[at:at + len(needle)] == needle:
                return marker
    return None


def main():
    table = load()
    failures = []
    if not table:
        failures.append("the marker list is empty")
    if table != [m.lower() for m in table]:
        failures.append("a marker is not lowercase")
    if len(set(table)) != len(table):
        failures.append("the marker list has duplicates")
    for marker in table:
        if marker != " ".join(marker.split()):
            failures.append(f"marker {marker!r} has stray whitespace")
        if not marker.replace(" ", "").isalpha():
            failures.append(f"marker {marker!r} is not alphabetic")

    for marker in table:
        probe = f"te lo digo, {marker}"
        if hits(probe, table) is None:
            failures.append(f"{marker!r} does not match itself as a whole word in {probe!r}")

    negatives = [
        "espero que estes bien",
        "el perro come en el patio",
        "la esperanza es lo ultimo que se pierde",
        "un tipo raro paso por aqui",
        "el causante del ruido era el viento",
        "el chambelán saludo a todos",
    ]
    for reply in negatives:
        matched = hits(reply, table)
        if matched is not None:
            failures.append(f"{reply!r} matched {matched!r} but holds no marker as a whole word")

    for failure in failures:
        print(failure)
    print(f"peruvian-markers: {len(table)} markers, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
