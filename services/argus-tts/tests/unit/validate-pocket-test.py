import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SERVICE = Path(__file__).resolve().parents[2]
TOOL = SERVICE / "tools" / "validate-pocket.py"
PROVISION = SERVICE / "scripts" / "provision.sh"
SPEC = importlib.util.spec_from_file_location("validate_pocket", TOOL)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def metadata(language):
    state = {
        "dtype": "float32", "fill": "zeros", "index": 0,
        "input_name": "state_0", "output_name": "out_state_0",
        "module": "transformer.layers.0.self_attn", "key": "cache",
        "path": "transformer.layers.0.self_attn/cache", "shape": [1, 2],
    }
    return {
        "schema_version": 2, "bundle_name": language, "language": language,
        "sample_rate": 24000, "frame_rate": 12.5, "samples_per_frame": 1920,
        "latent_dim": 32, "conditioning_dim": 1024, "max_token_per_chunk": 50,
        "pad_with_spaces_for_short_inputs": False, "remove_semicolons": False,
        "model_recommended_frames_after_eos": None, "insert_bos_before_voice": True,
        "tokenizer_file": "tokenizer.model", "bos_before_voice_file": "bos_before_voice.npy",
        "predefined_voices": ["jean"], "flow_lm_state_manifest": [state],
        "mimi_state_manifest": [copy.deepcopy(state)],
    }


class PocketValidationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pocket-validation-", dir="/tmp/opencode")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "artifact root"
        self.root.mkdir()
        self.manifest_path = self.base / "checksum manifest.json"
        self.manifest = {
            "schema_version": 1, "precision": "int8",
            "bundles": list(VALIDATOR.LANGUAGES), "assets": [],
        }
        for language in self.manifest["bundles"]:
            directory = f"onnx/{language}"
            self.add_asset((f"{directory}/bundle.json", json.dumps(metadata(language)).encode()))
            for name in (
                "flow_lm_main_int8.onnx", "flow_lm_flow_int8.onnx", "mimi_decoder_int8.onnx",
                "mimi_encoder.onnx", "text_conditioner.onnx", "tokenizer.model",
                "bos_before_voice.npy",
            ):
                self.add_asset((f"{directory}/{name}", f"synthetic {language} {name}".encode()))
            self.add_asset((f"languages/{language}/embeddings/jean.safetensors",
                            f"synthetic {language} Jean state".encode()))

    def add_asset(self, asset):
        name, content = asset
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        self.manifest["assets"].append({
            "path": name, "sha256": hashlib.sha256(content).hexdigest(),
        })

    def save_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def verify(self):
        self.save_manifest()
        return VALIDATOR.verify(self.root, self.manifest_path)

    def replace_metadata(self, value):
        name = "onnx/spanish/bundle.json"
        content = json.dumps(value).encode()
        (self.root / name).write_bytes(content)
        for asset in self.manifest["assets"]:
            if asset["path"] == name:
                asset["sha256"] = hashlib.sha256(content).hexdigest()

    def test_valid_synthetic_bilingual_int8(self):
        self.assertEqual(self.verify(), 18)

    def test_valid_synthetic_fp32(self):
        self.manifest["precision"] = "fp32"
        for asset in self.manifest["assets"]:
            if asset["path"].endswith("_int8.onnx"):
                old = self.root / asset["path"]
                asset["path"] = asset["path"].replace("_int8.onnx", ".onnx")
                old.rename(self.root / asset["path"])
        self.assertEqual(self.verify(), 18)

    def test_valid_single_language(self):
        self.manifest["bundles"] = ["spanish"]
        self.manifest["assets"] = [asset for asset in self.manifest["assets"]
                                   if "/spanish/" in asset["path"]]
        self.assertEqual(self.verify(), 9)

    def test_hashes_allow_uppercase(self):
        for asset in self.manifest["assets"]:
            asset["sha256"] = asset["sha256"].upper()
        self.assertEqual(self.verify(), 18)

    def test_corrupt_file(self):
        (self.root / self.manifest["assets"][1]["path"]).write_bytes(b"corrupt")
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "SHA256 mismatch"):
            self.verify()

    def test_missing_file(self):
        (self.root / self.manifest["assets"][1]["path"]).unlink()
        with self.assertRaises(FileNotFoundError):
            self.verify()

    def test_required_assets_must_be_listed(self):
        original = copy.deepcopy(self.manifest)
        for index in range(len(original["assets"])):
            with self.subTest(asset=original["assets"][index]["path"]):
                self.manifest = copy.deepcopy(original)
                del self.manifest["assets"][index]
                with self.assertRaisesRegex(VALIDATOR.ValidationError, "[Uu]nlisted|not listed"):
                    self.verify()

    def test_checks_extra_listed_files(self):
        self.add_asset(("NOTICE", b"synthetic notice"))
        (self.root / "NOTICE").write_bytes(b"corrupt")
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "SHA256 mismatch: NOTICE"):
            self.verify()

    def test_path_escapes_and_noncanonical_paths(self):
        for name in ("../outside", "/tmp/outside", "a/../../outside", "a/../bundle.json",
                     "./bundle.json", "a//b", "a/", "C:/outside", "a\\b", "", "a\x00b"):
            with self.subTest(path=name):
                self.manifest["assets"][0]["path"] = name
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_symlink_file(self):
        asset = self.manifest["assets"][1]
        path = self.root / asset["path"]
        outside = self.base / "outside"
        path.rename(outside)
        path.symlink_to(outside)
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Symlinks"):
            self.verify()

    def test_symlink_parent(self):
        path = self.root / "onnx"
        outside = self.base / "outside-directory"
        path.rename(outside)
        path.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Symlinks"):
            self.verify()

    def test_internal_symlink(self):
        asset = self.manifest["assets"][1]
        path = self.root / asset["path"]
        target = path.with_name("real.onnx")
        path.rename(target)
        path.symlink_to(target.name)
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Symlinks"):
            self.verify()

    def test_duplicate_paths(self):
        self.manifest["assets"].append(copy.deepcopy(self.manifest["assets"][0]))
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Duplicate asset path"):
            self.verify()

    def test_hardlink_alias(self):
        asset = self.manifest["assets"][1]
        os.link(self.root / asset["path"], self.root / "alias.onnx")
        self.manifest["assets"].append({"path": "alias.onnx", "sha256": asset["sha256"]})
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Aliased asset"):
            self.verify()

    def test_directory_is_not_asset(self):
        asset = self.manifest["assets"][1]
        path = self.root / asset["path"]
        path.unlink()
        path.mkdir()
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "regular file"):
            self.verify()

    def test_unsupported_language_and_duplicates(self):
        for languages in ([], ["english"], ["fr"], ["spanish_24l"], ["spanish", "spanish"],
                          [None], [{}], "spanish"):
            with self.subTest(languages=languages):
                self.manifest["bundles"] = languages
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_bad_manifest_schema(self):
        original = copy.deepcopy(self.manifest)
        for change in ({"schema_version": True}, {"schema_version": 2}, {"precision": "int4"},
                       {"assets": []}, {"assets": {}}, {"extra": 0}):
            with self.subTest(change=change):
                self.manifest = copy.deepcopy(original)
                self.manifest.update(change)
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()
        for field in original:
            with self.subTest(missing=field):
                self.manifest = copy.deepcopy(original)
                del self.manifest[field]
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_bad_asset_schema(self):
        original = copy.deepcopy(self.manifest)
        for asset in ({}, [], {"path": "a"}, {"sha256": "a"},
                      {"path": "a", "sha256": "z" * 64}, {"path": "a", "sha256": "0" * 63},
                      {"path": "a", "sha256": 0}, {"path": None, "sha256": "0" * 64}):
            with self.subTest(asset=asset):
                self.manifest = copy.deepcopy(original)
                self.manifest["assets"][0] = asset
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_metadata_missing_fields(self):
        original = metadata("spanish")
        for field in original:
            with self.subTest(field=field):
                value = copy.deepcopy(original)
                del value[field]
                self.replace_metadata(value)
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_metadata_schema_errors(self):
        for change in (
            {"schema_version": 1}, {"schema_version": True}, {"language": "english_2026-04"},
            {"bundle_name": "french"}, {"sample_rate": False}, {"latent_dim": -1},
            {"frame_rate": 0}, {"frame_rate": "12.5"}, {"samples_per_frame": 1},
            {"insert_bos_before_voice": "true"}, {"bos_before_voice_file": None},
            {"model_recommended_frames_after_eos": True}, {"predefined_voices": []},
            {"predefined_voices": ["jean", "jean"]}, {"flow_lm_state_manifest": []},
            {"mimi_state_manifest": {}}, {"tokenizer_file": "../escape"},
            {"bos_before_voice_file": "/outside"}, {"tokenizer_file": "missing.model"},
            {"bos_before_voice_file": "tokenizer.model"},
        ):
            with self.subTest(change=change):
                value = metadata("spanish")
                value.update(change)
                self.replace_metadata(value)
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_state_schema_errors(self):
        for change in (
            {"dtype": "float64"}, {"fill": "mixed"}, {"index": True}, {"index": 1},
            {"input_name": "state_7"}, {"output_name": "out_state_2"}, {"shape": [-1]},
            {"shape": [True]}, {"shape": "1"}, {"fill": "empty"}, {"shape": [0]},
            {"dtype": "int64", "fill": "nan"}, {"module": ""}, {"path": "wrong/cache"},
            {"extra": 0},
        ):
            with self.subTest(change=change):
                value = metadata("spanish")
                value["flow_lm_state_manifest"][0].update(change)
                self.replace_metadata(value)
                with self.assertRaises(VALIDATOR.ValidationError):
                    self.verify()

    def test_duplicate_state_path(self):
        value = metadata("spanish")
        entry = copy.deepcopy(value["flow_lm_state_manifest"][0])
        entry.update(index=1, input_name="state_1", output_name="out_state_1")
        value["flow_lm_state_manifest"].append(entry)
        self.replace_metadata(value)
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Duplicate state path"):
            self.verify()

    def test_empty_and_nan_states(self):
        value = metadata("spanish")
        value["flow_lm_state_manifest"][0].update(shape=[0], fill="empty")
        value["mimi_state_manifest"][0]["fill"] = "nan"
        self.replace_metadata(value)
        self.assertEqual(self.verify(), 18)

    def test_invalid_json_and_duplicate_keys(self):
        for content in ("{", "[]", '{"a": 1, "a": 2}', '{"a": NaN}', '{"a": Infinity}'):
            with self.subTest(content=content):
                self.manifest_path.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    VALIDATOR.verify(self.root, self.manifest_path)

    def test_metadata_duplicate_keys(self):
        name = "onnx/spanish/bundle.json"
        content = b'{"schema_version": 2, "schema_version": 2}'
        (self.root / name).write_bytes(content)
        for asset in self.manifest["assets"]:
            if asset["path"] == name:
                asset["sha256"] = hashlib.sha256(content).hexdigest()
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "Duplicate JSON key"):
            self.verify()

    def test_cli_and_opt_in_provision(self):
        self.save_manifest()
        for command in (
            [sys.executable, "-B", str(TOOL), str(self.root), str(self.manifest_path)],
            ["bash", str(PROVISION), "--validate-pocket", str(self.manifest_path), str(self.root)],
        ):
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("inference not validated", result.stdout)
                self.assertNotIn("Supertonic", result.stdout)

    def test_cli_failure_and_missing_arguments(self):
        self.save_manifest()
        (self.root / self.manifest["assets"][1]["path"]).unlink()
        for command in (
            [sys.executable, "-B", str(TOOL), str(self.root), str(self.manifest_path)],
            ["bash", str(PROVISION), "--validate-pocket", str(self.manifest_path), str(self.root)],
            ["bash", str(PROVISION), "--validate-pocket"],
        ):
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("Traceback", result.stderr)
                self.assertNotIn("Supertonic", result.stdout)

    def test_help_is_side_effect_free(self):
        for command in ([sys.executable, "-B", str(TOOL), "--help"],
                        ["bash", str(PROVISION), "--help"]):
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("manifest", result.stdout)
                self.assertNotIn("Setting up", result.stdout)


if __name__ == "__main__":
    unittest.main()
