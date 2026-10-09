#!/usr/bin/env python3
import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import tempfile
import tomllib

import onnxruntime

TEXTS = [
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
    "remind me to call mom at 5 pm",
    "what's on my calendar tomorrow?",
    "tell me a fun fact",
    "  espacios   dobles  y\ttabulador ",
    "emoji 😀 y ñandú, cigüeña, PINGÜINO",
    "",
]


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


def main():
    pin = pinned_onnxruntime()
    if onnxruntime.__version__ != pin:
        raise SystemExit(f"onnxruntime {onnxruntime.__version__} does not match the pinned {pin}")
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--agent-config", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    import laya
    from laya.common import encode_text

    bundle = pathlib.Path(args.bundle).resolve()
    agent_config = pathlib.Path(args.agent_config).resolve()
    with open(agent_config) as handle:
        config = json.load(handle)
    if config.get("option_layout", "sequential") != "sequential":
        raise SystemExit("the pilot parity reference supports the sequential option layout only")
    stage = pathlib.Path(tempfile.mkdtemp(prefix="laya-parity-"))
    try:
        shutil.copy(agent_config, stage / "rl_agent_config.json")
        shutil.copytree(bundle / "tokenizer", stage / "tokenizer")
        agent = laya.load(str(stage), backend="onnx", onnx_path=str(bundle / "model.onnx"))
        questions = json.loads((bundle / "questions.json").read_text())
        temperature = [float(value) for value in agent.cfg.get("temperature", [1.0, 1.0, 1.0])]
        cases = []
        for text in TEXTS:
            answers = agent.system_one(text, questions)["answers"]
            cases.append({
                "text": text,
                "tokens": encode_text(agent.tok, text, add_special_tokens=False)["input_ids"],
                "tool": answers["tool"]["probabilities"],
                "choice": answers["tool"]["choice"],
                "now": answers["now"]["noul"],
            })
        record = {
            "exporter": "services/llm/tests/eval/laya-pilot-parity.py",
            "backend": "laya " + getattr(laya, "__version__", "unknown") + " ONNXAgent",
            "bundle": str(bundle),
            "bundlePin": hashlib.sha256((bundle / "sha256").read_bytes()).hexdigest(),
            "modelSha256": sha256_of(bundle / "model.onnx"),
            "onnxruntime": onnxruntime.__version__,
            "questionsSha256": sha256_of(bundle / "questions.json"),
            "labelsSha256": sha256_of(bundle / "labels.json"),
            "agentConfig": str(agent_config),
            "parameters": {
                "maxLen": int(agent.cfg.get("max_len", 512)),
                "headMaxLen": int(agent.cfg.get("head_max_len", 192)),
                "temperature": temperature,
            },
            "cases": cases,
        }
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    pathlib.Path(args.out).write_text(json.dumps(record, ensure_ascii=False, indent=1) + "\n")
    print(f"wrote {len(cases)} cases to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
