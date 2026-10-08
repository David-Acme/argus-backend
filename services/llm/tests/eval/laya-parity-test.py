import hashlib
import json
import pathlib
import sys

FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "laya"
FIELDS = ("text", "tokens", "sequences", "tool", "now")


def main():
    provenance = json.loads((FIXTURES / "provenance.json").read_text())
    fixture = FIXTURES / provenance["fixture"]
    if not fixture.exists():
        print(f"SKIPPED: no frozen Laya fixture at {fixture}")
        return 77
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    problems = []
    if digest != provenance["fixtureSha256"]:
        problems.append(f"{fixture} hashes to {digest}, not the recorded {provenance['fixtureSha256']}")
    if not provenance.get("trainingRepoCommit"):
        problems.append("the provenance record names no training-repo commit")
    if not provenance.get("cppHalf"):
        problems.append("the provenance record does not say where the C++ half stands")
    if not provenance.get("questionSet", {}).get("sha256"):
        problems.append("the provenance record names no question set")
    cases = json.loads(fixture.read_text())["cases"]
    if len(cases) != provenance["cases"]:
        problems.append(f"the fixture holds {len(cases)} cases, not {provenance['cases']}")
    for index, case in enumerate(cases):
        for field in FIELDS:
            if field not in case:
                problems.append(f"case {index} has no {field}")
        if len(case["tokens"]) and not all(isinstance(token, int) for token in case["tokens"]):
            problems.append(f"case {index} tokens are not integers")
        for qid, sequence in case.get("sequences", {}).items():
            if "ids" not in sequence or "markers" not in sequence:
                problems.append(f"case {index} question {qid} has no ids or markers")
    if problems:
        for problem in problems:
            print(f"laya-parity-test: {problem}", file=sys.stderr)
        return 1
    print(f"laya-parity-test: ok ({len(cases)} frozen cases, {provenance['trainingRepoCommit'][:8]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
