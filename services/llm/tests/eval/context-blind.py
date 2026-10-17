#!/usr/bin/env python3
import argparse
import json
import pathlib
import random
import sys


def loadSamples(directory):
    pairs = []
    for samplePath in sorted(directory.glob("*.sample.jsonl")):
        keyPath = samplePath.with_name(samplePath.name[: -len(".sample.jsonl")] + ".key.jsonl")
        if not keyPath.exists():
            raise SystemExit(f"no key beside {samplePath}")
        keys = [json.loads(line) for line in keyPath.read_text(encoding="utf-8").splitlines() if line.strip()]
        texts = [json.loads(line) for line in samplePath.read_text(encoding="utf-8").splitlines() if line.strip()]
        if len(keys) != len(texts):
            raise SystemExit(f"{keyPath} maps {len(keys)} rows to {len(texts)} replies")
        label = samplePath.name[: -len(".sample.jsonl")]
        for key, text in zip(keys, texts):
            pairs.append((label, key, text["text"]))
    return pairs


def evenly(pairs, perVariant):
    byLabel = {}
    for label, key, text in pairs:
        byLabel.setdefault(label, []).append((label, key, text))
    chosen = []
    for label, rows in sorted(byLabel.items()):
        if len(rows) <= perVariant:
            chosen.extend(rows)
            continue
        stride = len(rows) / perVariant
        chosen.extend(rows[min(int(index * stride), len(rows) - 1)] for index in range(perVariant))
    return chosen


def write(out, keyOut, chosen, seed):
    random.Random(seed).shuffle(chosen)
    with open(out, "w", encoding="utf-8") as handle:
        for _, _, text in chosen:
            handle.write(json.dumps({"text": text}, ensure_ascii=False) + "\n")
    with open(keyOut, "w", encoding="utf-8") as handle:
        for line, (label, key, _) in enumerate(chosen, start=1):
            row = {"line": line, "variant": label, **key}
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")


def main(argv):
    parser = argparse.ArgumentParser(description="shuffle the blind sample of the context-scenarios runs")
    parser.add_argument("--dir", required=True, help="the directory the harnesses wrote their sample/key pairs into")
    parser.add_argument("--out", required=True, help="the one shuffled sample file (no variant labels)")
    parser.add_argument("--key", required=True, help="the key file that maps each shuffled line to its variant and case")
    parser.add_argument("--per-variant", type=int, default=40, help="replies to keep per model and context variant")
    parser.add_argument("--seed", type=int, default=7)
    options = parser.parse_args(argv)
    pairs = loadSamples(pathlib.Path(options.dir))
    if not pairs:
        raise SystemExit(f"no sample/key pairs under {options.dir}")
    chosen = evenly(pairs, options.per_variant)
    write(options.out, options.key, chosen, options.seed)
    variants = {}
    for label, _, _ in chosen:
        variants[label] = variants.get(label, 0) + 1
    for label, count in sorted(variants.items()):
        print(f"{label}: {count} replies")
    print(f"{len(chosen)} replies shuffled into {options.out}; key at {options.key}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
