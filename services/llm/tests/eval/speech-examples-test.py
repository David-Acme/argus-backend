#!/usr/bin/env python3
import json
import pathlib
import sys
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = HERE.parent / "fixtures" / "eval"
EXAMPLES = FIXTURES / "speech-examples.jsonl"
CASES = FIXTURES / "call-faithfulness.jsonl"

ACTS = ["ask_slot", "confirm", "choose", "done", "refused", "offer", "declined", "unactionable", "misunderstood"]


def load(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows


def fold(text):
    stripped = "".join(c for c in unicodedata.normalize("NFD", text.lower()) if unicodedata.category(c) != "Mn")
    return " ".join(stripped.split())


def contains(hay, needle):
    if not needle or len(needle) > len(hay):
        return False
    return any(hay[at:at + len(needle)] == needle for at in range(len(hay) - len(needle) + 1))


def identifier_shaped(token):
    for at in range(1, len(token) - 1):
        if token[at] in "._" and token[at - 1].isalnum() and token[at + 1].isalnum():
            return True
    return False


def main():
    examples = load(EXAMPLES)
    cases = load(CASES)
    failures = []

    seen = {}
    for row in examples:
        key = (row["act"], row["lang"], row["index"])
        if key in seen:
            failures.append(f"duplicate example {key}")
        seen[key] = row
        for field in ("user", "reply"):
            for token in row[field].split():
                if identifier_shaped(token):
                    failures.append(f"{key}: identifier-shaped token {token!r} in {field}")
    for act in ACTS:
        for lang in ("es", "en"):
            for index in (0, 1):
                if (act, lang, index) not in seen:
                    failures.append(f"missing example {(act, lang, index)}")

    utterances = []
    for case in cases:
        for utterance in case.get("script", []):
            utterances.append((case.get("id", ""), fold(utterance).split()))
    for row in examples:
        user = fold(row["user"]).split()
        if not user:
            continue
        for case_id, utterance in utterances:
            if contains(utterance, user) or contains(user, utterance):
                failures.append(f"example {row['act']}/{row['lang']}/{row['index']} user {user!r} overlaps case {case_id} {utterance!r}")

    for failure in failures:
        print(failure)
    print(f"speech-examples: {len(examples)} examples, {len(utterances)} case utterances, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
