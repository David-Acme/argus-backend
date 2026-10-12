import hashlib
import json
import pathlib
import sys

FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "laya"
PILOT_FIELDS = ("text", "tokens", "tool", "choice", "now")


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(problems, record, fields):
    fixture = FIXTURES / record["fixture"]
    if not fixture.exists():
        problems.append(f"SKIPPED: no frozen Laya fixture at {fixture}")
        return
    digest = sha256_of(fixture)
    if digest != record["fixtureSha256"]:
        problems.append(f"{fixture} hashes to {digest}, not the recorded {record['fixtureSha256']}")
    if not record.get("cppHalf"):
        problems.append(f"{fixture} has no cppHalf record")
    cases = json.loads(fixture.read_text())["cases"]
    if len(cases) != record["cases"]:
        problems.append(f"{fixture} holds {len(cases)} cases, not {record['cases']}")
    for index, case in enumerate(cases):
        for field in fields:
            if field not in case:
                problems.append(f"{fixture} case {index} has no {field}")
        if len(case["tokens"]) and not all(isinstance(token, int) for token in case["tokens"]):
            problems.append(f"{fixture} case {index} tokens are not integers")
        for label, probability in case["tool"].items():
            if not isinstance(probability, float) or not 0.0 <= probability <= 1.0:
                problems.append(f"{fixture} case {index} label {label} is not a probability")


def check_sequences(problems):
    fixture = FIXTURES / "sequence-parity.json"
    if not fixture.exists():
        problems.append(f"no frozen Laya sequence fixture at {fixture}")
        return
    root = json.loads(fixture.read_text())
    for key in ("exporter", "reference", "tokenizerSha256", "questionsSha256", "trainingRepoCommit"):
        if not root.get(key):
            problems.append(f"{fixture} records no {key}")
    questions = root.get("questions", {})
    if "tool" not in questions or "now" not in questions:
        problems.append(f"{fixture} does not render the shipped tool and now questions")
    for index, state in enumerate(root.get("states", [])):
        if not all(isinstance(token, int) for token in state.get("tokens", [])):
            problems.append(f"{fixture} state {index} tokens are not integers")
    cases = root.get("cases", [])
    if not cases:
        problems.append(f"{fixture} holds no sequence")
    for index, case in enumerate(cases):
        ids = case.get("ids", [])
        if case.get("question") not in questions:
            problems.append(f"{fixture} case {index} names a question the fixture does not hold")
        if not ids or not all(isinstance(token, int) for token in ids):
            problems.append(f"{fixture} case {index} has no integer ids")
        if len(ids) > case.get("maxLen", 0):
            problems.append(f"{fixture} case {index} is longer than its max_len")
        if not all(isinstance(marker, int) and marker < len(ids) for marker in case.get("markers", [])):
            problems.append(f"{fixture} case {index} has a marker outside its ids")


def main():
    problems = []
    check_sequences(problems)
    pilot_path = FIXTURES / "provenance-pilot.json"
    if pilot_path.exists():
        pilot = json.loads(pilot_path.read_text())
        if not pilot.get("parameters", {}).get("temperature"):
            problems.append("the pilot provenance record names no temperature")
        check(problems, pilot, PILOT_FIELDS)
    if problems:
        for problem in problems:
            print(f"laya-parity-test: {problem}", file=sys.stderr)
        return 1
    print("laya-parity-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
