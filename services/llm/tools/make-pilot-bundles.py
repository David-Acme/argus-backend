#!/usr/bin/env python3
import argparse
import hashlib
import json
import pathlib
import sys

LABELS = [
    ("memory_save", "the user states a fact to remember"),
    ("memory_recall", "ask what Argus remembers"),
    ("memory_forget", "forget something remembered"),
    ("reminder_set", "remind me at a time"),
    ("reminder_list", "list my reminders"),
    ("calendar_create", "schedule an event"),
    ("calendar_list", "what is on my agenda"),
    ("calendar_cancel", "cancel an event"),
    ("task_create", "create a task"),
    ("task_list", "list tasks"),
    ("task_complete", "mark a task done"),
    ("project_create", "create a project"),
    ("project_list", "list projects"),
    ("modules_list", "which modules exist"),
    ("modules_explain", "what a module does"),
    ("modules_enable", "turn a module on"),
    ("modules_disable", "turn a module off"),
    ("modules_purge", "delete a module's data"),
    ("app_open", "open an app screen"),
    ("app_guard_mode", "change the house security mode"),
    ("camera", "show a camera"),
    ("none", "not a command for Argus"),
]

LAYOUT = ("model.onnx", "tokenizer", "labels.json", "decision.json", "max_len", "model-card.md", "sha256", "manifest.json")


def sha256_of(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, payload):
    path.write_text(json.dumps(payload, indent=1, ensure_ascii=False) + "\n")


def seal(bundle):
    lines = []
    for path in sorted(bundle.rglob("*")):
        if path.is_file() and path.name != "sha256":
            lines.append(f"{sha256_of(path)}  {path.relative_to(bundle)}")
    (bundle / "sha256").write_text("".join(line + "\n" for line in lines))
    return [line.split("  ", 1)[1] for line in lines]


def laya(bundle):
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "model.onnx").write_bytes(b"pilot placeholder; a trained bundle replaces this file\n")
    tokenizer = bundle / "tokenizer"
    tokenizer.mkdir(exist_ok=True)
    (tokenizer / "tokenizer.json").write_text("{}\n")
    (tokenizer / "tokenizer_config.json").write_text("{}\n")
    write_json(bundle / "labels.json", {"labels": {label: text for label, text in LABELS}})
    write_json(bundle / "decision.json",
               {"act": 0.84, "ask": 0.84, "margin": 0.0, "now": 0.7, "guardMemory": False,
                "calibration": {"confidence": {"type": "identity"}, "now": {"type": "identity"}},
                "source": {"fitOn": "calibration", "note": "pilot placeholder until a trained bundle lands"}})
    (bundle / "max_len").write_text("512\n")
    (bundle / "model-card.md").write_text("# The Laya decision model, pilot placeholder\n")
    write_json(bundle / "manifest.json",
               {"model": "laya", "pilot": True, "artefact": "pilot placeholder",
                "corpus": "pilot", "corpusCommit": "pilot", "seed": 0,
                "calibration": {"fitOn": "calibration", "decision": "calibration"},
                "maxLen": 512, "labels": len(LABELS), "tokenizerFiles": [],
                "modelOnnx": {"bytes": (bundle / "model.onnx").stat().st_size, "sha256": sha256_of(bundle / "model.onnx")}})
    return seal(bundle)


def gliner(bundle):
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "model.onnx").write_bytes(b"pilot placeholder; a trained bundle replaces this file\n")
    tokenizer = bundle / "tokenizer"
    tokenizer.mkdir(exist_ok=True)
    (tokenizer / "tokenizer.json").write_text("{}\n")
    write_json(bundle / "labels.json", {"labels": ["title", "name", "project", "people", "when", "due_at"]})
    write_json(bundle / "decision.json",
               {"threshold": 0.5, "maxSpanWidth": 12,
                "source": {"fitOn": "calibration", "note": "pilot placeholder until a trained bundle lands"}})
    (bundle / "max_len").write_text("256\n")
    (bundle / "model-card.md").write_text("# The GLiNER extractor, pilot placeholder\n")
    write_json(bundle / "manifest.json",
               {"model": "gliner", "pilot": True, "artefact": "pilot placeholder",
                "corpus": "pilot", "corpusCommit": "pilot", "seed": 0,
                "calibration": {"fitOn": "calibration", "decision": "calibration"},
                "maxLen": 256, "labels": 6, "tokenizerFiles": [],
                "modelOnnx": {"bytes": (bundle / "model.onnx").stat().st_size, "sha256": sha256_of(bundle / "model.onnx")}})
    return seal(bundle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.root)
    for model, builder in (("decide", laya), ("extract", gliner)):
        bundle = root / model / "pilot"
        if bundle.exists():
            for path in sorted(bundle.rglob("*"), reverse=True):
                path.unlink() if path.is_file() else path.rmdir()
        files = builder(bundle)
        missing = [name for name in LAYOUT if not (bundle / name).exists()]
        if missing:
            raise SystemExit(f"{bundle} is incomplete: {', '.join(missing)}")
        print(json.dumps({"bundle": str(bundle), "files": len(files), "pin": sha256_of(bundle / "sha256")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
