import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parent / "context-blind.py"


def writeRun(directory, stem, count):
    sample = directory / f"{stem}.sample.jsonl"
    key = directory / f"{stem}.key.jsonl"
    with open(sample, "w", encoding="utf-8") as handle:
        for index in range(count):
            handle.write(json.dumps({"text": f"{stem} reply {index}"}) + "\n")
    with open(key, "w", encoding="utf-8") as handle:
        for index in range(count):
            handle.write(json.dumps({"id": f"case-{index}", "scenario": "panic", "locale": "es", "turn": index % 3}) + "\n")
    return sample, key


def run(directory, out, key, extra=()):
    return subprocess.run([sys.executable, "-I", str(TOOL), "--dir", str(directory), "--out", str(out),
                           "--key", str(key), *extra], capture_output=True, text=True, timeout=120)


class ContextBlindTest(unittest.TestCase):
    def test_the_sample_holds_the_text_only_and_the_key_maps_every_line(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = pathlib.Path(scratch)
            writeRun(root, "lfm2.5-1.2b.production", 100)
            writeRun(root, "lfm2.5-1.2b.oracle", 100)
            writeRun(root, "lfm2.5-2.6b.production", 12)
            out = root / "blind.jsonl"
            key = root / "blind-key.jsonl"
            process = run(root, out, key, ("--per-variant", "40", "--seed", "7"))
            self.assertEqual(process.returncode, 0, process.stderr)
            lines = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines() if line.strip()]
            keys = [json.loads(line) for line in key.read_text(encoding="utf-8").splitlines() if line.strip()]
            self.assertEqual(len(lines), 92)
            self.assertEqual(len(keys), 92)
            for row in lines:
                self.assertEqual(list(row), ["text"])
            for index, row in enumerate(keys, start=1):
                self.assertEqual(row["line"], index)
                self.assertIn(row["variant"], {"lfm2.5-1.2b.production", "lfm2.5-1.2b.oracle", "lfm2.5-2.6b.production"})
                self.assertIn("id", row)
            counts = {}
            for row in keys:
                counts[row["variant"]] = counts.get(row["variant"], 0) + 1
            self.assertEqual(counts["lfm2.5-1.2b.production"], 40)
            self.assertEqual(counts["lfm2.5-1.2b.oracle"], 40)
            self.assertEqual(counts["lfm2.5-2.6b.production"], 12)
            variants = {row["variant"] for row in keys}
            self.assertEqual(len(variants), 3)

    def test_the_shuffle_is_reproducible_and_the_key_follows_it(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = pathlib.Path(scratch)
            writeRun(root, "a.production", 10)
            writeRun(root, "b.oracle", 10)
            first = run(root, root / "one.jsonl", root / "one.key.jsonl", ("--seed", "5"))
            second = run(root, root / "two.jsonl", root / "two.key.jsonl", ("--seed", "5"))
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual((root / "one.jsonl").read_text(encoding="utf-8"),
                             (root / "two.jsonl").read_text(encoding="utf-8"))
            self.assertEqual((root / "one.key.jsonl").read_text(encoding="utf-8"),
                             (root / "two.key.jsonl").read_text(encoding="utf-8"))
            third = run(root, root / "three.jsonl", root / "three.key.jsonl", ("--seed", "6"))
            self.assertEqual(third.returncode, 0, third.stderr)
            self.assertNotEqual((root / "one.jsonl").read_text(encoding="utf-8"),
                                (root / "three.jsonl").read_text(encoding="utf-8"))

    def test_a_mismatched_pair_a_lone_sample_and_an_empty_directory_are_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = pathlib.Path(scratch)
            empty = run(root, root / "out.jsonl", root / "key.jsonl")
            self.assertNotEqual(empty.returncode, 0)
            self.assertIn("no sample/key pairs", empty.stderr)
            writeRun(root, "a.production", 5)
            (root / "a.production.sample.jsonl").write_text('{"text": "only one"}\n', encoding="utf-8")
            broken = run(root, root / "out.jsonl", root / "key.jsonl")
            self.assertNotEqual(broken.returncode, 0)
            self.assertIn("maps", broken.stderr)
            writeRun(root, "a.production", 5)
            (root / "lonely.sample.jsonl").write_text('{"text": "no key"}\n', encoding="utf-8")
            lonely = run(root, root / "out.jsonl", root / "key.jsonl")
            self.assertNotEqual(lonely.returncode, 0)
            self.assertIn("no key beside", lonely.stderr)


if __name__ == "__main__":
    unittest.main()
