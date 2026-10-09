#!/usr/bin/env python3
import importlib.util
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def load():
    spec = importlib.util.spec_from_file_location("speech_ttft_eval", HERE / "speech-ttft-eval.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_log(directory, name, text):
    path = pathlib.Path(directory) / name
    path.write_text(text, encoding="utf-8")
    return path


def main():
    module = load()
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        absent = write_log(directory, "absent.log", "argus-llm: the LLM loaded\n")
        both = write_log(directory,
                         "both.log",
                         "argus-llm: the Laya decider is ready on model.onnx with 22 options\n"
                         "argus-llm: the GLiNER extractor opened model.onnx with 5 types\n")
        cases = [
            (module.engines_up(None, []), None, "no requirement is not a refusal"),
            (module.engines_up(None, ["laya"]), "str", "a required engine with no log is refused"),
            (module.engines_up(str(absent), ["laya"]), "str", "a missing ready line is refused"),
            (module.engines_up(str(both), ["laya"]), None, "a present ready line passes"),
            (module.engines_up(str(both), ["laya", "gliner"]), None, "both present ready lines pass"),
            (module.engines_up(str(both), ["bogus"]), "str", "an unknown engine is refused"),
            (module.engines_up(str(absent), ["laya", "gliner"]), "str", "a partial read is refused"),
        ]
        for index, (result, expected, message) in enumerate(cases):
            if expected is None:
                if result is not None:
                    failures.append(f"case {index}: {message}: got {result!r}")
            elif not isinstance(result, str):
                failures.append(f"case {index}: {message}: got {result!r}")
    for failure in failures:
        print(failure)
    print(f"speech-ttft-guard: {len(cases)} cases, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
