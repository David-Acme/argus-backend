import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

import numpy as np
import onnxruntime
import torch
from onnxruntime.quantization import QuantType, quantize_dynamic
from torch import nn
from torch.nn import functional

import pocket_tts.modules.attention as attention
import pocket_tts.modules.conv as conv
import pocket_tts.modules.transformer as transformer
from pocket_tts.models.tts_model import TTSModel
from pocket_tts.modules.stateful_module import StatefulModule, increment_steps, init_states
from pocket_tts.utils.config import load_config

OPSET = 17
CONFIGS = Path(attention.__file__).resolve().parent.parent / "config"
TOLERANCE = 1e-3
MIMI_ROOTS = {"decoder", "decoder_transformer", "upsample"}


def scattered_append(self, k, v, state):
    if state is None:
        return PATCHES.original(attention._LinearKVCacheBackend, "append_and_get")(self, k, v, state)
    steps = k.shape[1]
    positions = state["offset"].view(1, 1, 1, 1, 1) + torch.arange(steps, dtype=torch.long).view(1, 1, steps, 1, 1)
    index = positions.expand(2, k.shape[0], steps, k.shape[2], k.shape[3])
    cache = state["cache"].scatter(2, index, torch.stack([k, v]))
    state["cache"] = cache
    end = state["offset"].view(()) + steps
    context = getattr(self, "context", None)
    start = torch.clamp(end - steps - context, min=0) if context is not None else torch.zeros_like(end)
    window = cache[:, :, start:end]
    k_attn = window[0].permute(0, 2, 1, 3)
    v_attn = window[1].permute(0, 2, 1, 3)
    pos_k = torch.arange(start, end, dtype=torch.long).view(1, -1).expand(k_attn.shape[0], -1)
    return k_attn, v_attn, pos_k, state["offset"]


def functional_increment(self, state, increment):
    state["offset"] = state["offset"] + increment


def functional_conv(self, x, model_state):
    width = self._effective_kernel_size - self._stride
    if model_state is None:
        if width:
            if self.pad_mode == "replicate":
                padding = x[..., :1].expand(x.shape[0], x.shape[1], width)
            else:
                padding = x.new_zeros(x.shape[0], x.shape[1], width)
            x = torch.cat([padding, x], dim=-1)
        return self.conv(x)
    if self.pad_mode != "constant":
        raise ValueError("Only constant padding is exportable for streaming state")
    state = self.get_state(model_state)
    if width:
        x = torch.cat([state["previous"], x], dim=-1)
    y = self.conv(x)
    if width:
        state["previous"] = x[..., -width:]
    return y


def functional_transposed_conv(self, x, model_state):
    state = self.get_state(model_state)
    partial = state["partial"]
    y = self.convtr(x)
    width = partial.shape[-1]
    if width == 0:
        return y
    head = y[..., :width] + partial
    tail = y[..., -width:]
    if self.convtr.bias is not None:
        tail = tail - self.convtr.bias[:, None]
    state["partial"] = tail
    return torch.cat([head, y[..., width:-width]], dim=-1)


def traced_causal_mask(steps, context, device):
    positions = torch.arange(steps, dtype=torch.long, device=device).view(1, -1)
    return attention._build_attention_mask(positions, positions, context)


def remembering_init(self, embed_dim, num_heads, rope, context=None):
    PATCHES.original(attention.StreamingMultiheadAttention, "__init__")(self, embed_dim, num_heads, rope, context)
    self._cache_backend.context = context


class Patches:
    def __init__(self, entries):
        self.entries = [(owner, name, getattr(owner, name), patched) for owner, name, patched in entries]

    def original(self, owner, name):
        return next(saved for target, attribute, saved, _ in self.entries if target is owner and attribute == name)

    def apply(self):
        for owner, name, _, patched in self.entries:
            setattr(owner, name, patched)

    def restore(self):
        for owner, name, saved, _ in self.entries:
            setattr(owner, name, saved)


PATCHES = Patches([
    (attention._LinearKVCacheBackend, "append_and_get", scattered_append),
    (attention._LinearKVCacheBackend, "increment_step", functional_increment),
    (attention.StreamingMultiheadAttention, "__init__", remembering_init),
    (conv.StreamingConv1d, "forward", functional_conv),
    (conv.StreamingConvTranspose1d, "forward", functional_transposed_conv),
    (transformer, "_cached_causal_mask", traced_causal_mask),
])


def flat_layout(module, capacity, roots=None):
    states = init_states(module, batch_size=1, sequence_length=capacity)
    layout = []
    for name in sorted(states):
        if roots is not None and name.split(".")[0] not in roots:
            continue
        for key in sorted(states[name]):
            tensor = states[name][key]
            if tensor.is_floating_point():
                tensor = torch.zeros_like(tensor)
            graph = key not in ("pad", "first") and tensor.numel() > 0
            layout.append({"module": name, "key": key, "tensor": tensor, "graph": graph})
    return layout


def graph_entries(layout):
    return [entry for entry in layout if entry["graph"]]


def rebuild(layout, tensors):
    state = {}
    inputs = iter(tensors)
    for entry in layout:
        value = next(inputs) if entry["graph"] else entry["tensor"].clone()
        state.setdefault(entry["module"], {})[entry["key"]] = value
    return state


def manifest(layout):
    entries = []
    for index, entry in enumerate(graph_entries(layout)):
        tensor = entry["tensor"]
        entries.append({
            "index": index,
            "module": entry["module"],
            "key": entry["key"],
            "input_name": f"state_{index}",
            "output_name": f"next_state_{index}",
            "shape": list(tensor.shape),
            "dtype": str(tensor.dtype).replace("torch.", ""),
            "fill": "ones" if tensor.dtype == torch.bool and bool(tensor.all()) else "zeros",
        })
    return entries


class TextConditioner(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.conditioner = model.flow_lm.conditioner

    def forward(self, token_ids):
        return self.conditioner(token_ids)


class FlowMain(nn.Module):
    def __init__(self, model, layout):
        super().__init__()
        self.flow_lm = model.flow_lm
        self.layout = layout

    def forward(self, sequence, text_embeddings, *states):
        state = rebuild(self.layout, list(states))
        flow_lm = self.flow_lm
        sequence = torch.where(torch.isnan(sequence), flow_lm.bos_emb, sequence)
        hidden = flow_lm.input_linear(sequence)
        out = flow_lm.backbone(hidden, text_embeddings, sequence, model_state=state)
        last = out[:, -1].to(torch.float32)
        eos = flow_lm.out_eos(last)
        increment_steps(flow_lm, state, increment=text_embeddings.shape[1] + sequence.shape[1])
        return (last, eos, *[state[e["module"]][e["key"]] for e in graph_entries(self.layout)])


class FlowStep(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.flow_net = model.flow_lm.flow_net

    def forward(self, c, s, t, x):
        return self.flow_net(c, s, t, x)


class MimiDecoder(nn.Module):
    def __init__(self, model, layout, steps_per_latent):
        super().__init__()
        self.mimi = model.mimi
        self.flow_lm = model.flow_lm
        self.layout = layout
        self.steps_per_latent = steps_per_latent

    def forward(self, latent, *states):
        state = rebuild(self.layout, list(states))
        scaled = latent * self.flow_lm.emb_std + self.flow_lm.emb_mean
        audio = self.mimi.decode_from_latent(scaled, state)
        for name, module in self.mimi.named_modules():
            if isinstance(module, StatefulModule) and name in state:
                module.increment_step(state[name], self.steps_per_latent * latent.shape[1])
        return (audio, *[state[e["module"]][e["key"]] for e in graph_entries(self.layout)])


class VoiceEncoder(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.mimi = model.mimi
        self.flow_lm = model.flow_lm

    def forward(self, audio):
        emb = self.mimi.encoder(audio, model_state=None)
        (emb,) = self.mimi.encoder_transformer(emb, None)
        emb = self.mimi._to_framerate(emb)
        prompt = functional.linear(emb.transpose(-1, -2).to(torch.float32), self.flow_lm.speaker_proj_weight)
        if self.flow_lm.insert_bos_before_voice:
            prompt = torch.cat([self.flow_lm.bos_before_voice, prompt], dim=1)
        return prompt


def export(module, arguments, path, names, axes):
    with torch.no_grad():
        torch.onnx.export(module, arguments, str(path), input_names=names[0], output_names=names[1],
                          dynamic_axes=axes, opset_version=OPSET, do_constant_folding=True, dynamo=False)


def session(path, threads):
    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = threads
    return onnxruntime.InferenceSession(str(path), options, providers=["CPUExecutionProvider"])


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load(variant, weights, tokenizer):
    config = load_config(CONFIGS / f"{variant}.yaml")
    config.weights_path = str(weights)
    config.weights_path_without_voice_cloning = None
    config.flow_lm.lookup_table.tokenizer_path = str(tokenizer)
    model = TTSModel._from_pydantic_config_with_weights(config, config.default_temperature, 1, None, -4.0)
    model.eval()
    return model, config


def check(label, expected, actual):
    error = float(np.max(np.abs(np.asarray(expected, dtype=np.float64) - np.asarray(actual, dtype=np.float64))))
    print(f"parity {label}: max abs error {error:.2e}")
    if not np.isfinite(error) or error > TOLERANCE:
        raise SystemExit(f"Pocket export failed its parity check on {label}")


def validate(model, folder, plan):
    generator = torch.Generator().manual_seed(1234)
    flow_layout, mimi_layout = plan["flow"], plan["mimi"]
    steps_per_latent, threads = plan["steps_per_latent"], plan["threads"]
    PATCHES.restore()
    try:
        with torch.no_grad():
            tokens = torch.randint(5, 3000, (1, 7), generator=generator)
            embeddings = model.flow_lm.conditioner(tokens)
            reference_state = init_states(model.flow_lm, batch_size=1, sequence_length=plan["flow_capacity"])
            latents = torch.randn(3, 1, 1, model.flow_lm.ldim, generator=generator)
            sequence_inputs = [torch.full((1, 1, model.flow_lm.ldim), float("nan"))] + [latents[i] for i in range(2)]
            expected_main = []
            empty_text = torch.zeros(1, 0, model.flow_lm.dim)
            empty_sequence = torch.zeros(1, 0, model.flow_lm.ldim)
            for sequence, text in [(empty_sequence, embeddings)] + [(s, empty_text) for s in sequence_inputs]:
                cleaned = torch.where(torch.isnan(sequence), model.flow_lm.bos_emb, sequence)
                out = model.flow_lm.backbone(model.flow_lm.input_linear(cleaned), text, cleaned, model_state=reference_state)
                last = out[:, -1].to(torch.float32)
                expected_main.append((last.numpy(), model.flow_lm.out_eos(last).numpy()))
                increment_steps(model.flow_lm, reference_state, increment=text.shape[1] + sequence.shape[1])
            flow_inputs = [torch.randn(1, model.flow_lm.dim, generator=generator), torch.full((1, 1), 0.0),
                           torch.full((1, 1), 1.0), torch.randn(1, model.flow_lm.ldim, generator=generator)]
            expected_flow = model.flow_lm.flow_net(*flow_inputs).numpy()
            mimi_state = init_states(model.mimi, batch_size=1, sequence_length=plan["mimi_capacity"])
            expected_audio = []
            for frame in latents:
                scaled = frame * model.flow_lm.emb_std + model.flow_lm.emb_mean
                expected_audio.append(model.mimi.decode_from_latent(scaled, mimi_state).numpy())
                increment_steps(model.mimi, mimi_state, increment=steps_per_latent)
    finally:
        PATCHES.apply()
    conditioner = session(folder / "text_conditioner.onnx", threads)
    check("text_conditioner", embeddings.numpy(), conditioner.run(None, {"token_ids": tokens.numpy()})[0])
    main = session(folder / "fp32" / "flow_lm_main.onnx", threads)
    states = [entry["tensor"].numpy().copy() for entry in graph_entries(flow_layout)]
    for index, (sequence, text) in enumerate([(empty_sequence, embeddings)] + [(s, empty_text) for s in sequence_inputs]):
        feeds = {"sequence": sequence.numpy(), "text_embeddings": text.numpy()}
        feeds.update({f"state_{i}": value for i, value in enumerate(states)})
        outputs = main.run(None, feeds)
        if index > 0:
            check(f"flow_lm_main step {index}", expected_main[index][0], outputs[0])
            check(f"flow_lm_main eos {index}", expected_main[index][1], outputs[1])
        states = outputs[2:]
    step = session(folder / "fp32" / "flow_lm_flow.onnx", threads)
    names = ["c", "s", "t", "x"]
    check("flow_lm_flow", expected_flow, step.run(None, {n: v.numpy() for n, v in zip(names, flow_inputs)})[0])
    decoder = session(folder / "fp32" / "mimi_decoder.onnx", threads)
    states = [entry["tensor"].numpy().copy() for entry in graph_entries(mimi_layout)]
    for index, frame in enumerate(latents):
        feeds = {"latent": frame.numpy()}
        feeds.update({f"state_{i}": value for i, value in enumerate(states)})
        outputs = decoder.run(None, feeds)
        check(f"mimi_decoder frame {index}", expected_audio[index], outputs[0])
        states = outputs[1:]


def check_encoder(model, folder, threads):
    generator = torch.Generator().manual_seed(99)
    frame = int(model.mimi.sample_rate / model.mimi.frame_rate)
    audio = torch.randn(1, 1, frame * 25, generator=generator) * 0.1
    PATCHES.restore()
    try:
        with torch.no_grad():
            expected = model._encode_audio(audio)
            if model.flow_lm.insert_bos_before_voice:
                expected = torch.cat([model.flow_lm.bos_before_voice, expected], dim=1)
    finally:
        PATCHES.apply()
    check("voice_encoder", expected.numpy(), session(folder / "voice_encoder.onnx", threads).run(None, {"audio": audio.numpy()})[0])


def main():
    parser = argparse.ArgumentParser(
        description="Export one Pocket TTS language model to the explicit-state ONNX bundle argus-tts runs.")
    parser.add_argument("--variant", required=True, help="pocket-tts config name, e.g. spanish, spanish_24l, english")
    parser.add_argument("--weights", required=True, type=Path, help="local model.safetensors")
    parser.add_argument("--tokenizer", required=True, type=Path, help="local tokenizer.json")
    parser.add_argument("--out", required=True, type=Path, help="empty output directory")
    parser.add_argument("--flow-capacity", type=int, default=640)
    parser.add_argument("--mimi-capacity", type=int, default=4096)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--cloning", action="store_true", help="also export the voice encoder (cloning weights only)")
    args = parser.parse_args()

    torch.manual_seed(0)
    PATCHES.apply()
    model, config = load(args.variant, args.weights, args.tokenizer)
    out = args.out
    fp32 = out / "fp32"
    fp32.mkdir(parents=True, exist_ok=True)

    tokens = torch.tensor([[5, 6, 7, 8]], dtype=torch.long)
    export(TextConditioner(model), (tokens,), out / "text_conditioner.onnx", (["token_ids"], ["text_embeddings"]),
           {"token_ids": {1: "tokens"}, "text_embeddings": {1: "tokens"}})

    flow_layout = flat_layout(model.flow_lm, args.flow_capacity)
    flow_states = graph_entries(flow_layout)
    export(FlowMain(model, flow_layout),
           (torch.zeros(1, 1, model.flow_lm.ldim), torch.zeros(1, 3, model.flow_lm.dim),
            *[entry["tensor"].clone() for entry in flow_states]),
           fp32 / "flow_lm_main.onnx",
           (["sequence", "text_embeddings", *[f"state_{i}" for i in range(len(flow_states))]],
            ["conditioning", "eos_logit", *[f"next_state_{i}" for i in range(len(flow_states))]]),
           {"sequence": {1: "sequence_length"}, "text_embeddings": {1: "text_length"}})

    export(FlowStep(model),
           (torch.zeros(1, model.flow_lm.dim), torch.zeros(1, 1), torch.ones(1, 1), torch.zeros(1, model.flow_lm.ldim)),
           fp32 / "flow_lm_flow.onnx", (["c", "s", "t", "x"], ["flow_dir"]), {})

    steps_per_latent = int(model.mimi.encoder_frame_rate / model.mimi.frame_rate)
    mimi_layout = flat_layout(model.mimi, args.mimi_capacity, MIMI_ROOTS)
    mimi_states = graph_entries(mimi_layout)
    export(MimiDecoder(model, mimi_layout, steps_per_latent),
           (torch.zeros(1, 2, model.flow_lm.ldim), *[entry["tensor"].clone() for entry in mimi_states]),
           fp32 / "mimi_decoder.onnx",
           (["latent", *[f"state_{i}" for i in range(len(mimi_states))]],
            ["audio", *[f"next_state_{i}" for i in range(len(mimi_states))]]),
           {"latent": {1: "frames"}, "audio": {2: "samples"}})

    if args.cloning:
        frame = int(model.mimi.sample_rate / model.mimi.frame_rate)
        export(VoiceEncoder(model), (torch.zeros(1, 1, frame * 8),), out / "voice_encoder.onnx",
               (["audio"], ["prompt"]), {"audio": {2: "samples"}, "prompt": {1: "frames"}})

    if args.cloning:
        check_encoder(model, out, args.threads)
    validate(model, out, {"flow": flow_states, "mimi": mimi_states, "steps_per_latent": steps_per_latent,
                          "threads": args.threads, "flow_capacity": args.flow_capacity,
                          "mimi_capacity": args.mimi_capacity})

    for name in ("flow_lm_main", "flow_lm_flow", "mimi_decoder"):
        quantize_dynamic(model_input=str(fp32 / f"{name}.onnx"), model_output=str(out / f"{name}_int8.onnx"),
                         weight_type=QuantType.QInt8, op_types_to_quantize=["MatMul"], per_channel=True)
    shutil.rmtree(fp32)
    shutil.copyfile(args.tokenizer, out / "tokenizer.json")

    bundle = {
        "schema_version": 3,
        "variant": args.variant,
        "tokenizer_file": "tokenizer.json",
        "temperature": config.default_temperature,
        "sampler_decode_steps": 1,
        "eos_threshold": -4.0,
        "min_frames_before_eos": TTSModel._MIN_FRAMES_BEFORE_EOS,
        "tokens_per_second_estimate": TTSModel._TOKENS_PER_SECOND_ESTIMATE,
        "gen_seconds_padding": TTSModel._GEN_SECONDS_PADDING,
        "sample_rate": model.mimi.sample_rate,
        "voice_cloning": bool(args.cloning),
        "samples_per_frame": int(model.mimi.sample_rate / model.mimi.frame_rate),
        "mimi_steps_per_latent": steps_per_latent,
        "latent_dim": model.flow_lm.ldim,
        "conditioning_dim": model.flow_lm.dim,
        "flow_capacity": args.flow_capacity,
        "mimi_capacity": args.mimi_capacity,
        "flow_lm_state_manifest": manifest(flow_layout),
        "mimi_state_manifest": manifest(mimi_layout),
        "replace_characters": config.replace_characters or {},
        "append_terminal_punctuation": config.append_terminal_punctuation,
        "capitalize_first_letter": config.capitalize_first_letter,
        "remove_semicolons": config.remove_semicolons,
        "pad_with_spaces_for_short_inputs": config.pad_with_spaces_for_short_inputs,
        "model_recommended_frames_after_eos": config.model_recommended_frames_after_eos,
    }
    (out / "bundle.json").write_text(json.dumps(bundle, indent=1))
    for path in sorted(out.iterdir()):
        if path.is_file():
            print(f"{path.name} {path.stat().st_size} {sha256(path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
