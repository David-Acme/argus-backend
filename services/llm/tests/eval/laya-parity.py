import argparse
import json
import pathlib
import sys

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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--onnx", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    import laya
    from laya.common import build_sequence, encode_text
    agent = laya.load(args.model, backend="onnx", onnx_path=args.onnx)
    questions = json.loads(pathlib.Path(args.model, "questions.json").read_text())
    internal = {qid: agent._to_internal(q) for qid, q in questions.items()}
    cases = []
    for text in TEXTS:
        answers = agent.system_one(text, questions)["answers"]
        sequences = {}
        for qid, q in internal.items():
            ids, markers = build_sequence(agent.tok, text, q, agent.cfg.get("max_len", 512), agent.cfg.get("head_max_len", 192))
            sequences[qid] = {"ids": ids, "markers": markers}
        cases.append({
            "text": text,
            "tokens": encode_text(agent.tok, text, add_special_tokens=False)["input_ids"],
            "sequences": sequences,
            "tool": answers["tool"]["probabilities"],
            "now": answers["now"]["noul"],
        })
    pathlib.Path(args.out).write_text(json.dumps({"cases": cases}, ensure_ascii=False, indent=1) + "\n")
    print(f"wrote {len(cases)} cases to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
