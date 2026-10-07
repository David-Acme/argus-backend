#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys

SKIP = 77
CLASSES = ("holder-direct", "other-direct", "side-talk", "background-tv", "background-noise")
SHORT = {
    "holder-direct": "holder",
    "other-direct": "other",
    "side-talk": "side-talk",
    "background-tv": "tv",
    "background-noise": "noise",
}
DIRECT = ("holder-direct", "other-direct")
DISTANT = ("side-talk", "background-tv", "background-noise")
BACKGROUND = ("background-tv", "background-noise")
FRR_DEFAULT = 0.01
DEFAULT_MANIFEST = (
    pathlib.Path(__file__).resolve().parents[1] / "fixtures" / "direct-speech" / "manifest.json"
)


def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(2)


def load_manifest(path):
    document = json.loads(path.read_text())
    clips = document["clips"]
    for clip in clips:
        for field in ("id", "class", "lang", "seconds", "source", "license", "sha256"):
            if field not in clip:
                fail(f"manifest entry {clip.get('id')!r} has no {field!r}")
        if clip["class"] not in CLASSES:
            fail(f"manifest entry {clip['id']!r} has unknown class {clip['class']!r}")
    return clips


def load_predictions(path, clips):
    predictions = {}
    for number, line in enumerate(path.read_text().splitlines(), start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except ValueError as error:
            fail(f"line {number} of {path} is not JSON: {error}")
        if not isinstance(row, dict):
            fail(f"line {number} of {path} is not a JSON object")
        identifier = row.get("id")
        if identifier is None:
            fail(f"line {number} of {path} has no 'id'")
        if "predicted" not in row and "class" not in row:
            fail(f"line {number} of {path} has no 'predicted' field for {identifier!r}")
        label = row.get("predicted", row.get("class"))
        if label not in CLASSES:
            fail(f"classification for {identifier!r} has unknown label {label!r}")
        if identifier in predictions:
            fail(f"classification for {identifier!r} appears twice")
        predictions[identifier] = label
    known = {clip["id"] for clip in clips}
    unknown = sorted(set(predictions) - known)
    if unknown:
        fail(f"{len(unknown)} classified ids are not in the manifest: {unknown[:5]}")
    missing = sorted(known - set(predictions))
    if missing:
        fail(f"{len(missing)} manifest ids were not classified: {missing[:5]}")
    return predictions


def rate(numerator, denominator):
    return numerator / denominator if denominator else 0.0


def score(clips, predictions):
    matrix = {truth: {predicted: 0 for predicted in CLASSES} for truth in CLASSES}
    for clip in clips:
        matrix[clip["class"]][predictions[clip["id"]]] += 1
    per_class = {}
    for label in CLASSES:
        support = sum(matrix[label].values())
        predicted = sum(matrix[truth][label] for truth in CLASSES)
        correct = matrix[label][label]
        per_class[label] = {
            "support": support,
            "predicted": predicted,
            "correct": correct,
            "precision": rate(correct, predicted),
            "recall": rate(correct, support),
        }
    direct_support = sum(matrix[label][predicted] for label in DIRECT for predicted in CLASSES)
    direct_missed = sum(matrix[label][predicted] for label in DIRECT for predicted in DISTANT)
    side_talk_support = sum(matrix["side-talk"].values())
    side_talk_as_direct = sum(matrix["side-talk"][label] for label in DIRECT)
    background_support = sum(matrix[label][predicted] for label in BACKGROUND for predicted in CLASSES)
    background_as_answer = sum(
        matrix[label][predicted] for label in BACKGROUND for predicted in DIRECT + ("side-talk",)
    )
    return {
        "clips": len(clips),
        "confusion": matrix,
        "perClass": per_class,
        "rates": {
            "directFalseReject": rate(direct_missed, direct_support),
            "sideTalkFalsePositive": rate(side_talk_as_direct, side_talk_support),
            "backgroundFalsePositive": rate(background_as_answer, background_support),
        },
        "support": {
            "direct": direct_support,
            "sideTalk": side_talk_support,
            "background": background_support,
        },
    }


def render(report, frr_target):
    lines = []
    rates = report["rates"]
    lines.append(
        f"clips={report['clips']} direct={report['support']['direct']} "
        f"sideTalk={report['support']['sideTalk']} background={report['support']['background']}"
    )
    header = f"{'class':<17}{'support':>8}{'predicted':>10}{'correct':>8}{'precision':>11}{'recall':>8}"
    lines.append(header)
    for label in CLASSES:
        row = report["perClass"][label]
        lines.append(
            f"{label:<17}{row['support']:>8}{row['predicted']:>10}{row['correct']:>8}"
            f"{row['precision']:>11.3f}{row['recall']:>8.3f}"
        )
    lines.append(f"{'':<18}" + "".join(f"{SHORT[label]:>10}" for label in CLASSES))
    for truth in CLASSES:
        cells = "".join(f"{report['confusion'][truth][predicted]:>10}" for predicted in CLASSES)
        lines.append(f"{truth:<18}{cells}")
    lines.append(
        f"direct-speech false reject={rates['directFalseReject']:.4f} target={frr_target:.4f} "
        f"side-talk false positive={rates['sideTalkFalsePositive']:.4f} "
        f"background false positive={rates['backgroundFalsePositive']:.4f}"
    )
    return "\n".join(lines)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="direct-speech-eval.py",
        description="Score a classifications file against the labelled direct-speech corpus.",
        epilog="classifications: JSONL, one {id, predicted} per line, every manifest id exactly once.",
    )
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--classifications")
    parser.add_argument("--report")
    parser.add_argument("--frr-target", type=float, default=FRR_DEFAULT)
    return parser.parse_args(argv)


def main(argv):
    options = parse_args(argv)
    manifest = pathlib.Path(options.manifest)
    if not manifest.exists():
        print(f"manifest not found: {manifest}", file=sys.stderr)
        return 2
    clips = load_manifest(manifest)
    if not options.classifications:
        print(
            f"no classifications input given; {len(clips)} labelled clips are ready under {manifest.parent}",
            file=sys.stderr,
        )
        return SKIP
    classifications = pathlib.Path(options.classifications)
    if not classifications.exists():
        print(f"classifications not found: {classifications}", file=sys.stderr)
        return 2
    predictions = load_predictions(classifications, clips)
    report = score(clips, predictions)
    report["frrTarget"] = options.frr_target
    report["frrWithinTarget"] = report["rates"]["directFalseReject"] <= options.frr_target
    print(render(report, options.frr_target))
    if options.report:
        try:
            pathlib.Path(options.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        except OSError as error:
            fail(f"cannot write the report to {options.report}: {error}")
    return 0 if report["frrWithinTarget"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
