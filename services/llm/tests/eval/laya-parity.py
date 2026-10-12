#!/usr/bin/env python3
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

STATES = [
    "recuérdame comprar pan mañana a las ocho",
    "¿qué tengo en la agenda hoy?",
    "pon la casa en modo noche",
    "muéstrame la cámara del patio",
    "hola Argus, ¿cómo estás?",
    "mi hermana se llama Lucía",
    "olvida lo que te dije de Lucía",
    "crea una tarea: llamar al plomero",
    "remind me to call mom at 5 pm",
    "what's on my calendar tomorrow?",
    "  espacios   dobles  y\ttabulador ",
    "emoji 😀 y ñandú, cigüeña, PINGÜINO",
    "un <mask> en medio y otro al final <mask>",
    "",
    " ".join(["la reunión del lunes con el equipo de ventas sigue en pie"] * 120),
]

BATTERY = {
    "choice-object": {
        "type": "choice",
        "instructions": "¿Qué pide el usuario?",
        "criteria": {
            "zeta": "último en el alfabeto",
            "alpha": None,
            "mid": "",
            "count": 3,
            "flag": True,
            "ratio": 0.5,
            "rubric": {"desc": "cámara del patio", "weight": 2.25, "tags": ["a", "ñ"], "none": None},
            "quote": "dice \"hola\"\ty\\n\u0001",
            "tiny": 1e-05,
            "huge": 1e16,
            "whole": 1.0,
        },
    },
    "choice-mask": {
        "type": "choice",
        "instructions": "pick <mask> one",
        "criteria": ["uno <mask> dos", "tres"],
    },
    "choice-long": {
        "type": "choice",
        "instructions": "Which of these many long options matches the request of the person speaking to the assistant?",
        "criteria": {
            f"option_{index:02d}": " ".join(["una descripción larga que repite palabras"] * (3 + index % 7))
            for index in range(30)
        },
    },
    "score": {
        "type": "score",
        "instructions": "How urgent is it?",
        "criteria": ["not at all", {"level": "some", "note": "ñ"}, 2.25, True, "very"],
    },
    "noul-labels": {
        "type": "noul",
        "instructions": {"ask": "¿está en casa?", "lang": "es", "n": 2},
        "criteria": {"True": "sí, está", "false": ""},
        "labels": {"false": " no ", "true": "sí"},
    },
    "noul-reversed": {
        "type": "noul",
        "instructions": ["is", "it", "late"],
        "criteria": {"true": "it is late", "false": {"why": "early"}},
    },
}

SHAPES = [(None, None), (64, 24), (40, 8)]


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def training_commit(root):
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--training", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    import laya
    import tokenizers
    import transformers
    from laya.agent import Agent
    from laya.common import build_sequence, encode_text

    bundle = pathlib.Path(args.bundle).resolve()
    decision = json.loads((bundle / "decision.json").read_text())
    max_len = int((bundle / "max_len").read_text().strip())
    head_max_len = int(decision["head_max_len"])
    tok = transformers.AutoTokenizer.from_pretrained(str(bundle / "tokenizer"))
    questions = json.loads((bundle / "questions.json").read_text())
    battery = dict(questions)
    battery.update(BATTERY)
    for qid, definition in battery.items():
        Agent._check_question(qid, definition)
    cases = []
    for qid, definition in battery.items():
        internal = Agent._to_internal(definition)
        for shape_max, shape_head in SHAPES:
            length = shape_max if shape_max is not None else max_len
            head = shape_head if shape_head is not None else head_max_len
            for state in STATES:
                ids, markers = build_sequence(tok, state, internal, length, head)
                cases.append({"question": qid, "state": state, "maxLen": length, "headMaxLen": head,
                              "ids": ids, "markers": markers})
    record = {
        "exporter": "services/llm/tests/eval/laya-parity.py",
        "reference": "laya.common.build_sequence " + getattr(laya, "__version__", "unknown"),
        "transformers": transformers.__version__,
        "tokenizers": tokenizers.__version__,
        "trainingRepoCommit": training_commit(args.training) if args.training else "",
        "bundle": bundle.name,
        "tokenizerSha256": sha256_of(bundle / "tokenizer" / "tokenizer.json"),
        "questionsSha256": sha256_of(bundle / "questions.json"),
        "maxLen": max_len,
        "headMaxLen": head_max_len,
        "questions": battery,
        "states": [{"text": state,
                    "tokens": encode_text(tok, state.replace(tok.mask_token, " "), add_special_tokens=False)["input_ids"]}
                   for state in STATES],
        "cases": cases,
    }
    head = {key: value for key, value in record.items() if key not in ("states", "cases")}
    lines = [json.dumps(head, ensure_ascii=False, indent=1)[:-2] + ","]
    for key in ("states", "cases"):
        rows = [" " + json.dumps(row, ensure_ascii=False, separators=(",", ":")) for row in record[key]]
        lines.append(f' "{key}": [')
        lines.append(",\n".join(rows))
        lines.append(" ]," if key == "states" else " ]")
    lines.append("}")
    pathlib.Path(args.out).write_text("\n".join(lines) + "\n")
    json.loads(pathlib.Path(args.out).read_text())
    print(f"wrote {len(cases)} sequences to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
