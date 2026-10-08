import hashlib
import json
import pathlib
import sys

FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "laya"
FIELDS = ("text", "tokens", "sequences", "tool", "now")
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
        for qid, sequence in case.get("sequences", {}).items():
            if "ids" not in sequence or "markers" not in sequence:
                problems.append(f"{fixture} case {index} question {qid} has no ids or markers")
        if fields is PILOT_FIELDS:
            for label, probability in case["tool"].items():
                if not isinstance(probability, float) or not 0.0 <= probability <= 1.0:
                    problems.append(f"{fixture} case {index} label {label} is not a probability")


def main():
    problems = []
    rounds = json.loads((FIXTURES / "provenance.json").read_text())
    if rounds.get("fixture"):
        if not rounds.get("trainingRepoCommit") or not rounds.get("questionSet", {}).get("sha256"):
            problems.append("the provenance record names no training-repo commit or question set")
        check(problems, rounds, FIELDS)
    pilot_path = FIXTURES / "provenance-pilot.json"
    if pilot_path.exists():
        pilot = json.loads(pilot_path.read_text())
        if not pilot.get("bundlePin") or not pilot.get("parameters", {}).get("temperature"):
            problems.append("the pilot provenance record names no bundle pin or temperature")
        check(problems, pilot, PILOT_FIELDS)
    if problems:
        for problem in problems:
            print(f"laya-parity-test: {problem}", file=sys.stderr)
        return 1
    print("laya-parity-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
