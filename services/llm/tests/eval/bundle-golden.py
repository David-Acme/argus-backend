#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import pathlib
import shutil
import sys
import tempfile
import tomllib

PROBES = [
    "recuérdame comprar pan mañana a las ocho",
    "¿qué tengo en la agenda hoy?",
    "pon la casa en modo noche",
    "muéstrame la cámara del patio",
    "hola Argus, ¿cómo estás?",
    "mi hermana se llama Lucía",
    "¿cómo se llama mi hermana?",
    "olvida lo que te dije de Lucía",
    "crea una tarea: llamar al plomero",
    "marca como hecha la tarea del plomero",
    "activa el módulo de productividad",
    "abre la pantalla de ajustes",
    "agéndame una reunión con Andrea el jueves a las tres",
    "apunta que el gimnasio abre a las siete",
    "remind me to call mom at 5 pm",
    "what's on my calendar tomorrow?",
    "tell me a fun fact",
    "emoji 😀 y ñandú, cigüeña, PINGÜINO",
]


def usage():
    return """bundle-golden.py --kind decide|extract --bundle DIR --precision fp32|int8 [--rows ROWS] [tolerances]

Writes the bundle's own golden vectors (golden.json) from the model it carries, records them and the
precision in manifest.json, and reseals the bundle's sha256 listing. A decider's probes are fixed; an
extractor's are the rows a GLiNER fixture export writes (--rows). Run it on a freshly exported bundle,
never on an installed slot."""


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def pinned_onnxruntime():
    lock = pathlib.Path(__file__).resolve().parent / "parity-references.lock.toml"
    with lock.open("rb") as handle:
        return tomllib.load(handle)["onnxruntime"]


def laya_probes(bundle):
    import laya
    from laya.common import build_sequence

    decision = json.loads((bundle / "decision.json").read_text())
    max_len = int((bundle / "max_len").read_text().strip())
    stage = pathlib.Path(tempfile.mkdtemp(prefix="bundle-golden-"))
    try:
        shutil.copytree(bundle / "tokenizer", stage / "tokenizer")
        (stage / "rl_agent_config.json").write_text(json.dumps({
            "encoder": "tokenizer", "max_len": max_len, "head_max_len": decision["head_max_len"],
            "temperature": decision["temperature"]}))
        agent = laya.load(str(stage), backend="onnx", onnx_path=str(bundle / "model.onnx"))
        questions = json.loads((bundle / "questions.json").read_text())
        internal = {qid: agent._to_internal(q) for qid, q in questions.items()}
        probes = []
        for text in PROBES:
            answers = agent.system_one(text, questions)["answers"]
            sequences = {}
            for qid, q in internal.items():
                ids, markers = build_sequence(agent.tok, text, q, max_len, decision["head_max_len"])
                sequences[qid] = {"ids": ids, "markers": markers}
            probes.append({"text": text, "lang": "es", "sequences": sequences, "tool": answers["tool"]["probabilities"],
                           "choice": answers["tool"]["choice"], "now": answers["now"]["noul"]})
        return probes, "laya " + getattr(laya, "__version__", "unknown") + " ONNXAgent"
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def gliner_probes(bundle, rows_path):
    import numpy
    import onnxruntime
    from gliner2.inference.overlap import resolve_overlaps

    decision = json.loads((bundle / "decision.json").read_text())
    threshold = float(decision["pairThreshold"])
    max_words = int(decision["maxSpanWords"])
    session = onnxruntime.InferenceSession(str(bundle / "model.onnx"), providers=["CPUExecutionProvider"])
    probes = []
    for case in json.loads(pathlib.Path(rows_path).read_text())["cases"]:
        feed = {name: numpy.array([case[key]], dtype=numpy.int64) for name, key in (
            ("input_ids", "inputIds"), ("attention_mask", "attentionMask"), ("word_indices", "wordIndices"),
            ("word_mask", "wordMask"), ("query_indices", "queryIndices"), ("query_mask", "queryMask"))}
        indices, mask, logits = session.run(["candidate_indices", "candidate_mask", "pair_logits"], feed)
        reference = {}
        for query, field in enumerate(case["fields"]):
            raw = []
            for slot in range(mask.shape[2]):
                if not mask[0][query][slot]:
                    continue
                start, end = int(indices[0][query][slot][0]), int(indices[0][query][slot][1])
                if end - start > max_words:
                    continue
                logit = float(logits[0][query][slot])
                probability = 1.0 / (1.0 + math.exp(-logit)) if logit >= 0 else math.exp(logit) / (1.0 + math.exp(logit))
                if probability >= threshold:
                    raw.append((probability, start, end))
            chosen = resolve_overlaps(raw, "flat", score=lambda i: i[0], start=lambda i: i[1], end=lambda i: i[2])
            spans = []
            for probability, start, end in chosen:
                if not (0 <= start < end <= len(case["wordStarts"])):
                    continue
                begin, stop = int(case["wordStarts"][start]), int(case["wordEnds"][end - 1])
                surface = case["text"][begin:stop].strip()
                if surface:
                    spans.append({"text": surface, "confidence": probability, "start": begin, "end": stop})
            if spans:
                reference[field] = spans
        probes.append({key: case[key] for key in ("id", "text", "lang", "type", "fields", "inputIds", "wordIndices",
                                                  "wordMask", "queryIndices", "queryMask")} | {"reference": reference})
    return probes, "onnxruntime " + onnxruntime.__version__


def main():
    parser = argparse.ArgumentParser(usage=usage())
    parser.add_argument("--kind", required=True, choices=["decide", "extract"])
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--precision", required=True, choices=["fp32", "int8"])
    parser.add_argument("--rows")
    parser.add_argument("--tolerance-probability", type=float, default=2e-3)
    parser.add_argument("--tolerance-now", type=float, default=2e-3)
    parser.add_argument("--tolerance-score", type=float, default=2e-3)
    args = parser.parse_args()
    import onnxruntime
    if onnxruntime.__version__ != pinned_onnxruntime():
        raise SystemExit(f"onnxruntime {onnxruntime.__version__} is not the {pinned_onnxruntime()} argus-llm links")
    bundle = pathlib.Path(args.bundle).resolve()
    if args.kind == "decide":
        probes, reference = laya_probes(bundle)
        kind, tolerance = "laya", {"probability": args.tolerance_probability, "now": args.tolerance_now}
    else:
        if not args.rows:
            raise SystemExit("an extractor's probes are the rows of a GLiNER fixture export: pass --rows")
        probes, reference = gliner_probes(bundle, args.rows)
        kind, tolerance = "gliner", {"score": args.tolerance_score}
    golden = bundle / "golden.json"
    golden.write_text(json.dumps({"kind": kind, "reference": reference, "modelSha256": sha256_of(bundle / "model.onnx"),
                                  "probes": probes}, ensure_ascii=False, indent=1) + "\n")
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["precision"] = args.precision
    manifest["golden"] = {"file": "golden.json", "sha256": sha256_of(golden), "tolerance": tolerance}
    manifest_path.write_text(json.dumps(manifest, indent=1, ensure_ascii=False, sort_keys=True) + "\n")
    names = sorted(path.relative_to(bundle).as_posix() for path in bundle.rglob("*") if path.is_file() and path.name != "sha256")
    (bundle / "sha256").write_text("".join(f"{sha256_of(bundle / name)}  {name}\n" for name in names))
    print(f"wrote {len(probes)} {kind} probes to {golden} and resealed {bundle.name}", file=sys.stderr)


if __name__ == "__main__":
    main()
