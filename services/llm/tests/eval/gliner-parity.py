import argparse
import json
import pathlib
import sys

CASES = [
    "agenda una cita con el dentista el martes a las cuatro",
    "crea una tarea: llamar al plomero",
    "recuérdame comprar pan mañana a las ocho",
    "qué tengo en la agenda hoy",
    "remind me to call mom at five",
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", required=True)
    parser.add_argument("--rows", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--threshold", type=float, default=0.5)
    parser.add_argument("--max-span-width", type=int, default=12)
    args = parser.parse_args()
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import extract_schemas as schemas
    import gliner_filler
    import gliner_runtime
    import types

    options = types.SimpleNamespace(mode="record", cardinality="optional_one",
                                    threshold=args.threshold, descriptions=False)
    model = gliner_runtime.load(threads=1, adapter=args.adapter)
    rows = [json.loads(line) for line in pathlib.Path(args.rows).read_text().splitlines() if line.strip()]
    by_text = {row["text"]: row for row in rows}
    cases = []
    for text in CASES:
        type_name = schemas.LABELS[by_text[text]["intent"]][0] if text in by_text else "calendar_create"
        fields = gliner_filler.record_values(model, type_name, text, options)
        cases.append({"text": text, "type": type_name, "fields": fields})
    pathlib.Path(args.out).write_text(json.dumps({"cases": cases}, ensure_ascii=False, indent=1) + "\n")
    print(f"wrote {len(cases)} cases to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
