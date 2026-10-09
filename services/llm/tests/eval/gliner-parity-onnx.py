import argparse
import hashlib
import json
import math
import pathlib
import sys
import tomllib

import numpy as np
import onnxruntime

from gliner2.inference.overlap import resolve_overlaps


def pinned_onnxruntime():
    lock = pathlib.Path(__file__).resolve().parent / "parity-references.lock.toml"
    with lock.open("rb") as handle:
        return tomllib.load(handle)["onnxruntime"]


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def sigmoid(value):
    if value >= 0.0:
        return 1.0 / (1.0 + math.exp(-value))
    exp = math.exp(value)
    return exp / (1.0 + exp)


def flat(spans):
    return [(p, s, e) for p, s, e in resolve_overlaps(
        spans, "flat", score=lambda i: i[0], start=lambda i: i[1], end=lambda i: i[2])]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--rows", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    pin = pinned_onnxruntime()
    if onnxruntime.__version__ != pin:
        raise SystemExit(f"onnxruntime {onnxruntime.__version__} does not match the pinned {pin}")

    bundle = pathlib.Path(args.bundle).resolve()
    rows_path = pathlib.Path(args.rows).resolve()
    rows = json.loads(rows_path.read_text())
    threshold = float(rows["pairThreshold"])
    max_words = int(rows["maxSpanWords"])

    session = onnxruntime.InferenceSession(str(bundle / "model.onnx"), providers=["CPUExecutionProvider"])
    cases = []
    for case in rows["cases"]:
        feed = {
            "input_ids": np.array([case["inputIds"]], dtype=np.int64),
            "attention_mask": np.array([case["attentionMask"]], dtype=np.int64),
            "word_indices": np.array([case["wordIndices"]], dtype=np.int64),
            "word_mask": np.array([case["wordMask"]], dtype=np.int64),
            "query_indices": np.array([case["queryIndices"]], dtype=np.int64),
            "query_mask": np.array([case["queryMask"]], dtype=np.int64),
        }
        indices, mask, logits, _ = session.run(
            ["candidate_indices", "candidate_mask", "pair_logits", "null_logits"], feed)
        starts = case["wordStarts"]
        ends = case["wordEnds"]
        text = case["text"]
        by_field = {}
        for query, field in enumerate(case["fields"]):
            raw = []
            for slot in range(mask.shape[2]):
                if not mask[0][query][slot]:
                    continue
                start = int(indices[0][query][slot][0])
                end = int(indices[0][query][slot][1])
                if end - start > max_words:
                    continue
                probability = sigmoid(float(logits[0][query][slot]))
                if probability < threshold:
                    continue
                raw.append((probability, start, end))
            chosen = flat([tuple(entry) for entry in raw])
            out = []
            for probability, start, end in chosen:
                if not (0 <= start < end <= len(starts)):
                    continue
                begin = int(starts[start])
                stop = int(ends[end - 1])
                surface = text[begin:stop].strip()
                if not surface:
                    continue
                out.append({"text": surface, "confidence": probability, "start": begin, "end": stop})
            if out:
                by_field[field] = out
        cases.append({"id": case["id"], "reference": by_field})

    record = {
        "provenance": {
            "reference": "python onnxruntime over the shipped bundle's own model.onnx on the frozen rows",
            "runtime": "onnxruntime " + onnxruntime.__version__,
            "decoder": "gliner2.inference.overlap.resolve_overlaps policy=flat",
            "model": "model.onnx",
            "modelSha256": sha256_of(bundle / "model.onnx"),
            "bundlePin": hashlib.sha256((bundle / "sha256").read_bytes()).hexdigest(),
            "rows": rows_path.name,
            "rowsCases": len(rows["cases"]),
            "pairThreshold": threshold,
            "maxSpanWords": max_words,
            "sigmoid": "1/(1+exp(-logit)) in python float from the graph's float32 pair_logits",
        },
        "cases": cases,
    }
    pathlib.Path(args.out).write_text(json.dumps(record, ensure_ascii=False, indent=1) + "\n")
    print(f"wrote {len(cases)} cases to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
