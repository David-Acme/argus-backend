import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys


LANGUAGES = ("english_2026-04", "spanish")
MODEL_STEMS = ("flow_lm_main", "flow_lm_flow", "mimi_decoder")


class ValidationError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ValidationError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValidationError(f"Invalid JSON constant: {value}")


def load_json(path):
    return json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
        parse_constant=reject_constant,
    )


def relative_path(value):
    require(isinstance(value, str) and bool(value), "Expected a nonempty asset path")
    require(
        not any(char in value for char in "\\:\x00")
        and all(part not in ("", ".", "..") for part in value.split("/")),
        f"Unsafe or noncanonical path: {value!r}",
    )
    path = PurePosixPath(value)
    require(not path.is_absolute(), f"Absolute asset path: {value!r}")
    return path


def local_file(root, name):
    relative = relative_path(name)
    candidate = root
    for part in relative.parts:
        candidate = candidate / part
        require(not candidate.is_symlink(), f"Symlinks are not accepted: {name}")
    resolved = candidate.resolve(strict=True)
    require(resolved.is_relative_to(root), f"Asset escapes root: {name}")
    require(resolved.is_file(), f"Asset is not a regular file: {name}")
    return resolved


def validate_states(entries):
    require(isinstance(entries, list) and bool(entries), "Missing state manifest entries")
    paths = set()
    for index, entry in enumerate(entries):
        require(isinstance(entry, dict), "State entry must be an object")
        require(
            set(entry) == {
                "dtype", "fill", "index", "input_name", "key", "module",
                "output_name", "path", "shape",
            },
            "Unexpected state entry fields",
        )
        require(type(entry["index"]) is int and entry["index"] == index,
                "State indices must be contiguous and ordered")
        require(entry["input_name"] == f"state_{index}"
                and entry["output_name"] == f"out_state_{index}",
                "State input/output names do not match index")
        for field in ("key", "module", "path"):
            require(isinstance(entry[field], str) and bool(entry[field]),
                    f"Invalid state {field}")
        require(entry["path"] == f'{entry["module"]}/{entry["key"]}',
                "State path does not match module/key")
        require(entry["path"] not in paths, "Duplicate state path")
        paths.add(entry["path"])
        require(entry["dtype"] in ("float32", "float16", "int64", "bool"),
                "Unsupported state dtype")
        shape = entry["shape"]
        require(isinstance(shape, list)
                and all(type(dim) is int and dim >= 0 for dim in shape),
                "Invalid state shape")
        fill = entry["fill"]
        require(fill in ("nan", "ones", "zeros", "empty"), "Unsupported state fill")
        require((fill == "empty") == (0 in shape), "Empty state fill/shape mismatch")
        require(fill != "nan" or entry["dtype"] in ("float32", "float16"),
                "NaN fill requires floating point state")


def validate_metadata(metadata, language):
    require(isinstance(metadata, dict), "bundle.json must be an object")
    require(type(metadata.get("schema_version")) is int
            and metadata["schema_version"] == 2, "Only bundle schema_version 2 is supported")
    require(metadata.get("bundle_name") == language and metadata.get("language") == language,
            "Bundle language/name mismatch")
    for field in ("sample_rate", "samples_per_frame", "latent_dim", "conditioning_dim",
                  "max_token_per_chunk"):
        require(type(metadata.get(field)) is int and metadata[field] > 0,
                f"Invalid bundle {field}")
    rate = metadata.get("frame_rate")
    require(type(rate) in (int, float) and math.isfinite(rate) and rate > 0,
            "Invalid frame_rate")
    require(math.isclose(metadata["sample_rate"] / rate, metadata["samples_per_frame"]),
            "Inconsistent frame/sample rates")
    for field in ("insert_bos_before_voice", "pad_with_spaces_for_short_inputs",
                  "remove_semicolons"):
        require(type(metadata.get(field)) is bool, f"Invalid bundle {field}")
    require("model_recommended_frames_after_eos" in metadata, "Missing EOS metadata")
    frames = metadata["model_recommended_frames_after_eos"]
    require(frames is None or (type(frames) is int and frames >= 0), "Invalid EOS metadata")
    voices = metadata.get("predefined_voices")
    require(isinstance(voices, list)
            and all(isinstance(voice, str) and bool(voice) for voice in voices),
            "Invalid predefined_voices")
    require(len(voices) == len(set(voices)) and "jean" in voices, "Jean voice not declared")
    for field in ("flow_lm_state_manifest", "mimi_state_manifest"):
        validate_states(metadata.get(field))
    tokenizer = relative_path(metadata.get("tokenizer_file"))
    require("bos_before_voice_file" in metadata, "Missing BOS metadata")
    bos = metadata["bos_before_voice_file"]
    require(not metadata["insert_bos_before_voice"] or bool(bos), "Missing BOS file")
    files = [str(tokenizer)]
    if bos is not None:
        files.append(str(relative_path(bos)))
    require(len(files) == len(set(files)), "Duplicate tokenizer/BOS paths")
    return files


def verify(root, manifest_path):
    root = Path(root).resolve(strict=True)
    require(root.is_dir(), "Root must be a directory")
    manifest = load_json(Path(manifest_path))
    require(isinstance(manifest, dict)
            and set(manifest) == {"schema_version", "precision", "bundles", "assets"},
            "Expected schema_version, precision, bundles and assets")
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == 1,
            "Only checksum manifest schema_version 1 is supported")
    precision = manifest["precision"]
    require(precision in ("int8", "fp32"), "Unsupported precision")
    languages = manifest["bundles"]
    require(isinstance(languages, list) and bool(languages)
            and all(language in LANGUAGES for language in languages),
            "Supported bundles: english_2026-04, spanish")
    require(len(languages) == len(set(languages)), "Duplicate bundle language")
    assets = manifest["assets"]
    require(isinstance(assets, list) and bool(assets), "Assets must be a nonempty list")
    files = {}
    identities = set()
    for asset in assets:
        require(isinstance(asset, dict) and set(asset) == {"path", "sha256"},
                "Each asset requires exactly path and sha256")
        name = str(relative_path(asset["path"]))
        require(name not in files, f"Duplicate asset path: {name}")
        digest = asset["sha256"]
        require(isinstance(digest, str) and re.fullmatch(r"[0-9a-fA-F]{64}", digest),
                f"Invalid SHA256: {name}")
        path = local_file(root, name)
        stat = path.stat()
        identity = (stat.st_dev, stat.st_ino)
        require(identity not in identities, f"Aliased asset: {name}")
        identities.add(identity)
        files[name] = (path, digest.lower())
    for name, (path, expected) in files.items():
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        require(digest.hexdigest() == expected, f"SHA256 mismatch: {name}")
    for language in languages:
        directory = f"onnx/{language}"
        bundle = f"{directory}/bundle.json"
        require(bundle in files, f"Unlisted bundle metadata: {bundle}")
        metadata_files = validate_metadata(load_json(files[bundle][0]), language)
        suffix = "_int8" if precision == "int8" else ""
        required = [f"{directory}/{stem}{suffix}.onnx" for stem in MODEL_STEMS]
        required.extend(f"{directory}/{name}" for name in (
            "mimi_encoder.onnx", "text_conditioner.onnx", *metadata_files,
        ))
        required.append(f"languages/{language}/embeddings/jean.safetensors")
        for name in required:
            require(name in files, f"Required asset is not listed: {name}")
    return len(files)


def main():
    parser = argparse.ArgumentParser(
        description="Offline Pocket ONNX checksum and bundle-metadata validation (stdlib only).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''Checksum manifest (separate from upstream bundle.json):
  {"schema_version": 1, "precision": "int8", "bundles": ["english_2026-04", "spanish"],
   "assets": [{"path": "<root-relative file>", "sha256": "<64 real hex digits>"}, ...]}
Use int8 or fp32; only the two listed bundle names are supported (no aliases).
Root overlays the published layouts: onnx/<language>/bundle.json, selected
flow_lm_main/flow_lm_flow/mimi_decoder[_int8].onnx, mimi_encoder.onnx,
text_conditioner.onnx, the metadata's tokenizer/BOS files, plus
languages/<language>/embeddings/jean.safetensors from kyutai/pocket-tts.
List and hash every required file, including bundle.json and Jean per language.
All listed assets are checked; extra unlisted files are not certified. Paths
must be canonical relative POSIX paths; symlinks and hardlink aliases are refused.
The manifest is caller-trusted: obtain hashes from trusted, pinned revisions.
No downloads, inference, config writes, ONNX/tokenizer/voice tensor compatibility
checks or model-quality certification. Synthetic test success is not model validation.
Upstream references:
  https://huggingface.co/KevinAHM/pocket-tts-onnx
  https://github.com/KevinAHM/pocket-tts-onnx-export
Built-in voice lookup uses the network; a later runtime must use local Jean states.''',
    )
    parser.add_argument("root", type=Path, help="Local artifact root (not a runtime config)")
    parser.add_argument("manifest", type=Path, help="Caller-supplied checksum JSON path")
    args = parser.parse_args()
    try:
        count = verify(args.root, args.manifest)
    except (ValueError, OSError, RuntimeError, RecursionError) as error:
        print(f"Pocket validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Checked {count} asset SHA256 values and bundle metadata; inference not validated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
