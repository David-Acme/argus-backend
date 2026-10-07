#!/usr/bin/env python3
import argparse
import array
import hashlib
import json
import math
import pathlib
import subprocess
import sys
import tarfile
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT_DEFAULT = REPO_ROOT / "services" / "voice" / "tests" / "fixtures" / "direct-speech"
CACHE_DEFAULT = REPO_ROOT / "models" / "voice-fixtures-cache"

SAMPLE_RATE = 16000
BYTES_PER_SECOND = 2 * SAMPLE_RATE
DIRECT_RMS_DB = -20.0
PEAK_CEIL_DB = -1.0
SPEAKER_OFFSET_DB = 0.0
SECOND_SPEAKER_OFFSET_DB = -1.5

COMMON_VOICE_REVISION = "8262c16bf297c87a9cd88c51997c4758ed7a8ba2"
COMMON_VOICE_DATASET = "https://huggingface.co/datasets/fsicoli/common_voice_17_0"
COMMON_VOICE_BASE = f"{COMMON_VOICE_DATASET}/resolve/{COMMON_VOICE_REVISION}"
COMMON_VOICE_ES_SHARD = f"{COMMON_VOICE_BASE}/audio/es/test/es_test_0.tar"
COMMON_VOICE_URL = f"{COMMON_VOICE_DATASET}/tree/{COMMON_VOICE_REVISION}/audio/es/test"
COMMON_VOICE_LICENSE = "CC0-1.0"
COMMON_VOICE_SOURCE = "common-voice-17.0-es-test"

LIBRISPEECH_URL = "https://www.openslr.org/resources/31/dev-clean-2.tar.gz"
LIBRISPEECH_SHA256 = "176ec501490eced2d6c1f89f4f0ddc7dfe799e649e5322f8ba49fe3ff50c8012"
LIBRISPEECH_LICENSE = "CC-BY-4.0"
LIBRISPEECH_SOURCE = "mini-librispeech-dev-clean-2"

CLASSES = ("holder-direct", "other-direct", "side-talk", "background-tv", "background-noise")

TV_LOWPASS_HZ = 2800
TV_ECHO = "aecho=0.8:0.85:45|95:0.35|0.2"
TV_BED_RMS_DB = -44.0
TV_FLOOR_RMS_DB = -55.0

MUSIC_ROOTS = (
    (196.0, 246.9, 293.7, 3.4),
    (220.0, 277.2, 329.6, 4.1),
    (174.6, 220.0, 261.6, 2.7),
)

CLIPS = (
    {
        "id": "holder-en-01", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0000", "start": 0.15, "length": 2.40,
    },
    {
        "id": "holder-en-02", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0001", "start": 0.20, "length": 3.20,
    },
    {
        "id": "holder-en-03", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0006", "start": 0.10, "length": 2.30,
    },
    {
        "id": "holder-en-04", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0007", "start": 0.10, "length": 2.20,
    },
    {
        "id": "holder-en-05", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0011", "start": 0.10, "length": 2.80,
    },
    {
        "id": "holder-en-06", "class": "holder-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694",
        "file": "5694-64038-0012", "start": 0.10, "length": 2.60,
    },
    {
        "id": "holder-es-01", "class": "holder-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:0f0a130d83e6",
        "file": "common_voice_es_30785056", "start": 0.25, "length": 3.10,
    },
    {
        "id": "holder-es-02", "class": "holder-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:0f0a130d83e6",
        "file": "common_voice_es_30785060", "start": 0.30, "length": 3.10,
    },
    {
        "id": "holder-es-03", "class": "holder-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:0f0a130d83e6",
        "file": "common_voice_es_30785247", "start": 0.25, "length": 2.90,
    },
    {
        "id": "holder-es-04", "class": "holder-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:0f0a130d83e6",
        "file": "common_voice_es_30785354", "start": 0.30, "length": 3.40,
    },
    {
        "id": "other-en-01", "class": "other-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "3000",
        "file": "3000-15664-0004", "start": 0.10, "length": 2.60,
    },
    {
        "id": "other-en-02", "class": "other-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "3000",
        "file": "3000-15664-0006", "start": 0.10, "length": 2.40,
    },
    {
        "id": "other-en-03", "class": "other-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "3000",
        "file": "3000-15664-0026", "start": 0.10, "length": 2.40,
    },
    {
        "id": "other-en-04", "class": "other-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "3000",
        "file": "3000-15664-0030", "start": 0.10, "length": 2.80,
    },
    {
        "id": "other-en-05", "class": "other-direct", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "3000",
        "file": "3000-15664-0000", "start": 0.10, "length": 2.90,
    },
    {
        "id": "other-es-01", "class": "other-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:044f9f36b423",
        "file": "common_voice_es_31174909", "start": 0.20, "length": 2.80,
    },
    {
        "id": "other-es-02", "class": "other-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:5d8b422cbdc3",
        "file": "common_voice_es_34351414", "start": 0.20, "length": 3.70,
    },
    {
        "id": "other-es-03", "class": "other-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:597035e334d9",
        "file": "common_voice_es_31097782", "start": 0.20, "length": 3.50,
    },
    {
        "id": "other-es-04", "class": "other-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:332959162164",
        "file": "common_voice_es_23860516", "start": 0.20, "length": 3.00,
    },
    {
        "id": "other-es-05", "class": "other-direct", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:8f923f1b2fe5",
        "file": "common_voice_es_36603868", "start": 0.10, "length": 2.40,
    },
    {
        "id": "sidetalk-en-01", "class": "side-talk", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694+3000",
        "turns": (
            ("5694-64038-0009", "first", 0.30, 1.20),
            ("3000-15664-0006", "second", 0.30, 1.40),
            ("5694-64038-0000", "first", 0.30, 1.20),
        ),
    },
    {
        "id": "sidetalk-en-02", "class": "side-talk", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694+3000",
        "turns": (
            ("3000-15664-0004", "second", 0.40, 1.30),
            ("5694-64038-0012", "first", 0.40, 1.35),
            ("3000-15664-0026", "second", 0.20, 1.15),
        ),
    },
    {
        "id": "sidetalk-en-03", "class": "side-talk", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694+3000",
        "turns": (
            ("5694-64038-0013", "first", 0.50, 1.25),
            ("3000-15664-0030", "second", 0.30, 1.30),
            ("5694-64038-0016", "first", 0.20, 1.15),
        ),
    },
    {
        "id": "sidetalk-en-04", "class": "side-talk", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "5694+7850",
        "turns": (
            ("7850-281318-0003", "second", 0.40, 1.25),
            ("5694-64038-0021", "first", 0.40, 1.30),
            ("7850-281318-0015", "second", 0.30, 1.15),
        ),
    },
    {
        "id": "sidetalk-es-01", "class": "side-talk", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:044f9f36b423+cv-es:5d8b422cbdc3",
        "turns": (
            ("common_voice_es_31174904", "first", 0.30, 1.30),
            ("common_voice_es_34351416", "second", 0.30, 1.40),
            ("common_voice_es_31174909", "first", 0.30, 1.20),
        ),
    },
    {
        "id": "sidetalk-es-02", "class": "side-talk", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:eb22edd60361+cv-es:597035e334d9",
        "turns": (
            ("common_voice_es_30744629", "first", 0.40, 1.30),
            ("common_voice_es_31097785", "second", 0.40, 1.35),
            ("common_voice_es_30744630", "first", 0.30, 1.20),
        ),
    },
    {
        "id": "sidetalk-es-03", "class": "side-talk", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:6e41405699ac+cv-es:0f0a130d83e6",
        "turns": (
            ("common_voice_es_19600493", "first", 0.50, 1.25),
            ("common_voice_es_30785354", "second", 0.60, 1.30),
            ("common_voice_es_19600495", "first", 0.40, 1.20),
        ),
    },
    {
        "id": "sidetalk-es-04", "class": "side-talk", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:0f0a130d83e6+cv-es:5d8b422cbdc3",
        "turns": (
            ("common_voice_es_30785247", "first", 0.80, 1.25),
            ("common_voice_es_34351412", "second", 0.50, 1.30),
            ("common_voice_es_30785060", "first", 0.90, 1.15),
        ),
    },
    {
        "id": "tv-en-01", "class": "background-tv", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "7850",
        "file": "7850-281318-0000", "start": 0.20, "length": 3.40, "distance_db": -14.0, "bed": 0,
    },
    {
        "id": "tv-en-02", "class": "background-tv", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "7850",
        "file": "7850-281318-0001", "start": 0.20, "length": 3.50, "distance_db": -16.0, "bed": 1,
    },
    {
        "id": "tv-en-03", "class": "background-tv", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "7850",
        "file": "7850-281318-0002", "start": 0.20, "length": 3.40, "distance_db": -18.0, "bed": 2,
    },
    {
        "id": "tv-en-04", "class": "background-tv", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "7850",
        "file": "7850-281318-0007", "start": 0.20, "length": 3.40, "distance_db": -20.0, "bed": 0,
    },
    {
        "id": "tv-en-05", "class": "background-tv", "lang": "en",
        "source": LIBRISPEECH_SOURCE, "speaker": "7850",
        "file": "7850-281318-0016", "start": 0.10, "length": 3.00, "distance_db": -18.0, "bed": 1,
    },
    {
        "id": "tv-es-01", "class": "background-tv", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:652df565de28",
        "file": "common_voice_es_19629310", "start": 0.20, "length": 3.60, "distance_db": -14.0, "bed": 2,
    },
    {
        "id": "tv-es-02", "class": "background-tv", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:d821bd5bbd48",
        "file": "common_voice_es_23746397", "start": 0.20, "length": 3.50, "distance_db": -16.0, "bed": 0,
    },
    {
        "id": "tv-es-03", "class": "background-tv", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:a398cd09293b",
        "file": "common_voice_es_19728407", "start": 0.20, "length": 3.60, "distance_db": -18.0, "bed": 1,
    },
    {
        "id": "tv-es-04", "class": "background-tv", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:128b429b286e",
        "file": "common_voice_es_37923305", "start": 0.20, "length": 3.40, "distance_db": -20.0, "bed": 2,
    },
    {
        "id": "tv-es-05", "class": "background-tv", "lang": "es",
        "source": COMMON_VOICE_SOURCE, "speaker": "cv-es:9a04ff3d1f0f",
        "file": "common_voice_es_39761932", "start": 0.20, "length": 3.50, "distance_db": -18.0, "bed": 0,
    },
    {
        "id": "noise-01", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 3.00, "target_rms_db": -14.0,
        "hum_hz": 52.0, "hum_db": -26.0, "clink_hz": 2400.0, "clink_at": 0.90, "seed": 11,
    },
    {
        "id": "noise-02", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 3.40, "target_rms_db": -18.0,
        "hum_hz": 58.0, "hum_db": -28.0, "clink_hz": 3100.0, "clink_at": 1.60, "seed": 12,
    },
    {
        "id": "noise-03", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 2.60, "target_rms_db": -22.0,
        "hum_hz": 50.0, "hum_db": -24.0, "clink_hz": 1800.0, "clink_at": 1.10, "seed": 13,
    },
    {
        "id": "noise-04", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 3.80, "target_rms_db": -26.0,
        "hum_hz": 54.0, "hum_db": -27.0, "clink_hz": 4200.0, "clink_at": 2.20, "seed": 14,
    },
    {
        "id": "noise-05", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 2.80, "target_rms_db": -30.0,
        "hum_hz": 60.0, "hum_db": -25.0, "clink_hz": 2700.0, "clink_at": 1.30, "seed": 15,
    },
    {
        "id": "noise-06", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 3.20, "target_rms_db": -34.0,
        "hum_hz": 49.0, "hum_db": -29.0, "clink_hz": 3600.0, "clink_at": 0.70, "seed": 16,
    },
    {
        "id": "noise-07", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 3.60, "target_rms_db": -38.0,
        "hum_hz": 56.0, "hum_db": -26.0, "clink_hz": 2100.0, "clink_at": 1.90, "seed": 17,
    },
    {
        "id": "noise-08", "class": "background-noise", "lang": "none",
        "source": "synthetic", "speaker": "", "seconds": 2.40, "target_rms_db": -42.0,
        "hum_hz": 62.0, "hum_db": -30.0, "clink_hz": 1500.0, "clink_at": 1.40, "seed": 18,
    },
)


def ffmpeg(args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)


def ffmpeg_version():
    result = subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True, check=True)
    return result.stdout.splitlines()[0].removeprefix("ffmpeg version ").split(" ")[0]


def pcm_seconds(path):
    return path.stat().st_size / BYTES_PER_SECOND


def level(pcm_path):
    samples = array.array("h")
    samples.frombytes(pcm_path.read_bytes())
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max(abs(value) for value in samples) / 32768.0
    rms = math.sqrt(sum(float(value) * value for value in samples) / len(samples)) / 32768.0
    return 20.0 * math.log10(peak), 20.0 * math.log10(rms)


def level_gain(pcm_path, target_rms_db):
    peak_db, rms_db = level(pcm_path)
    return min(target_rms_db - rms_db, PEAK_CEIL_DB - peak_db)


def decode(source, target):
    if not source.exists():
        raise SystemExit(f"source {source} is missing from the cache; drop --skip-fetch to fetch it")
    if target.exists():
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    raw = ["-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1"] if source.suffix == ".pcm" else []
    ffmpeg([*raw, "-i", str(source), "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "s16le", str(target)])
    return target


def extract(source, start, length, target, gain_db=0.0, tail_filters=()):
    fade_out = max(length - 0.06, 0.0)
    chain = [f"volume={gain_db:.3f}dB", *tail_filters, "afade=t=in:st=0:d=0.02",
             f"afade=t=out:st={fade_out:.3f}:d=0.06"]
    ffmpeg(["-ss", f"{start:.3f}", "-t", f"{length:.3f}", "-f", "s16le",
            "-ar", str(SAMPLE_RATE), "-ac", "1", "-i", str(source),
            "-af", ",".join(chain), "-f", "s16le", str(target)])
    return target


def silence(seconds):
    return bytes(int(round(seconds * SAMPLE_RATE)) * 2)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url, target):
    request = urllib.request.Request(url, headers={"User-Agent": "argus-voice-fixtures"})
    with urllib.request.urlopen(request, timeout=300) as response:
        target.write_bytes(response.read())
    return target


def fetch_common_voice(cache, identifiers, staging):
    pending = {name for name in identifiers if not (cache / f"{name}.pcm").exists()}
    if not pending:
        return
    cache.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(COMMON_VOICE_ES_SHARD, headers={"User-Agent": "argus-voice-fixtures"})
    with urllib.request.urlopen(request, timeout=600) as response:
        with tarfile.open(fileobj=response, mode="r|") as archive:
            for member in archive:
                name = pathlib.Path(member.name).stem
                if name not in pending:
                    continue
                payload = archive.extractfile(member).read()
                source = staging / f"{name}.mp3"
                source.write_bytes(payload)
                decode(source, cache / f"{name}.pcm")
                source.unlink()
                pending.discard(name)
                if not pending:
                    break
    if pending:
        raise SystemExit(f"clips missing from the Common Voice shard: {sorted(pending)}")


def fetch_librispeech(cache, names):
    pending = {name for name in names if not (cache / f"{name}.flac").exists()}
    if not pending:
        return
    cache.mkdir(parents=True, exist_ok=True)
    archive_path = cache / "dev-clean-2.tar.gz"
    if not archive_path.exists():
        download(LIBRISPEECH_URL, archive_path)
    if digest(archive_path) != LIBRISPEECH_SHA256:
        raise SystemExit(f"{archive_path} does not match the pinned dev-clean-2 sha256")
    with tarfile.open(archive_path, "r|gz") as archive:
        for member in archive:
            name = pathlib.Path(member.name).stem
            if not member.isfile() or name not in pending:
                continue
            (cache / f"{name}.flac").write_bytes(archive.extractfile(member).read())
            pending.discard(name)
            if not pending:
                break
    if pending:
        raise SystemExit(f"clips missing from the LibriSpeech archive: {sorted(pending)}")


def cache_folder(source, cache_root):
    return cache_root / ("librispeech" if source == LIBRISPEECH_SOURCE else "common-voice-es")


def decode_source(source, name, cache_root, staging):
    suffix = ".flac" if source == LIBRISPEECH_SOURCE else ".pcm"
    return decode(cache_folder(source, cache_root) / f"{name}{suffix}", staging / f"{name}.pcm")


def music_bed(cache, index, seconds):
    target = cache / f"music-bed-{index}-{seconds:.2f}.raw"
    if target.exists():
        return target
    low, mid, high, depth = MUSIC_ROOTS[index % len(MUSIC_ROOTS)]
    roots = (low, mid, high)
    inputs = []
    for root in roots:
        inputs.extend(["-f", "lavfi", "-i", f"sine=frequency={root}:duration={seconds:.3f}"])
    graph = (
        f"[0:a]volume=0.25[a0];[1:a]volume=0.25[a1];[2:a]volume=0.25[a2];"
        f"[a0][a1][a2]amix=inputs=3:normalize=0:duration=first,"
        f"tremolo=f={depth:.1f}:d=0.4,lowpass=f=5000[a]")
    ffmpeg([*inputs, "-filter_complex", graph, "-map", "[a]", "-t", f"{seconds:.3f}",
            "-ar", str(SAMPLE_RATE), "-ac", "1", "-f", "s16le", str(target)])
    return target


def build_direct(clip, source, staging, out_dir):
    target = out_dir / f"{clip['id']}.bin"
    raw = staging / f"{clip['id']}-raw.bin"
    extract(source, clip["start"], clip["length"], raw)
    gain = level_gain(raw, DIRECT_RMS_DB)
    extract(source, clip["start"], clip["length"], target, gain_db=gain)
    raw.unlink()
    processing = (
        f"trim {clip['start']:.2f}-{clip['start'] + clip['length']:.2f}s; "
        f"level-normalised to {DIRECT_RMS_DB:.1f} dBFS RMS target; 16 kHz mono s16le")
    return processing


def build_side_talk(clip, cache, staging, out_dir):
    parts = []
    descriptors = []
    for index, (name, role, start, length) in enumerate(clip["turns"]):
        source = decode_source(clip["source"], name, cache, staging)
        raw = staging / f"{clip['id']}-turn{index}-raw.bin"
        extract(source, start, length, raw)
        offset = SPEAKER_OFFSET_DB if role == "first" else SECOND_SPEAKER_OFFSET_DB
        gain = level_gain(raw, DIRECT_RMS_DB + offset)
        turn = staging / f"{clip['id']}-turn{index}.bin"
        extract(source, start, length, turn, gain_db=gain)
        raw.unlink()
        parts.append(turn.read_bytes())
        if index + 1 < len(clip["turns"]):
            parts.append(silence(0.45 if index == 0 else 0.50))
        descriptors.append(f"{name.rsplit('-', 1)[0]} {start:.2f}+{length:.2f}s")
        turn.unlink()
    body = b"".join(parts)
    (out_dir / f"{clip['id']}.bin").write_bytes(body)
    processing = (
        f"{len(clip['turns'])} turns ({', '.join(descriptors)}) with 0.45/0.50s gaps; "
        f"per-turn level-normalised to {DIRECT_RMS_DB:.1f}/"
        f"{DIRECT_RMS_DB + SECOND_SPEAKER_OFFSET_DB:.1f} dBFS RMS target; 16 kHz mono s16le")
    return processing


def build_tv(clip, source, cache, staging, out_dir):
    raw = staging / f"{clip['id']}-raw.bin"
    extract(source, clip["start"], clip["length"], raw)
    gain = level_gain(raw, DIRECT_RMS_DB) + clip["distance_db"]
    raw.unlink()
    bed = music_bed(cache, clip["bed"], clip["length"])
    floor_source = staging / "tv-floor.raw"
    if not floor_source.exists():
        ffmpeg(["-f", "lavfi", "-i", f"anoisesrc=color=pink:sample_rate={SAMPLE_RATE}:"
                                     f"amplitude=0.05:duration=30:seed=7",
                "-ar", str(SAMPLE_RATE), "-ac", "1", "-f", "s16le", str(floor_source)])
    bed_gain = TV_BED_RMS_DB - level(bed)[1]
    floor_gain = TV_FLOOR_RMS_DB - level(floor_source)[1]
    graph = (
        f"[0:a]volume={gain:.3f}dB,lowpass=f={TV_LOWPASS_HZ},{TV_ECHO}[speech];"
        f"[1:a]volume={bed_gain:.3f}dB[music];"
        f"[2:a]volume={floor_gain:.3f}dB,atrim=0:{clip['length']:.3f}[floor];"
        f"[speech][music][floor]amix=inputs=3:normalize=0:duration=first,"
        f"afade=t=in:st=0:d=0.02,afade=t=out:st={max(clip['length'] - 0.06, 0.0):.3f}:d=0.06[a]")
    ffmpeg(["-ss", f"{clip['start']:.3f}", "-t", f"{clip['length']:.3f}", "-f", "s16le",
            "-ar", str(SAMPLE_RATE), "-ac", "1", "-i", str(source),
            "-t", f"{clip['length']:.3f}", "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1",
            "-i", str(bed),
            "-t", f"{clip['length']:.3f}", "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1",
            "-i", str(floor_source),
            "-filter_complex", graph, "-map", "[a]", "-t", f"{clip['length']:.3f}",
            "-f", "s16le", str(out_dir / f"{clip['id']}.bin")])
    processing = (
        f"trim {clip['start']:.2f}-{clip['start'] + clip['length']:.2f}s; "
        f"{clip['distance_db']:.0f} dB distance from {DIRECT_RMS_DB:.1f} dBFS RMS direct "
        f"(speech at {DIRECT_RMS_DB + clip['distance_db']:.1f} dBFS RMS target); "
        f"lowpass {TV_LOWPASS_HZ} Hz; aecho 45/95 ms; music bed {TV_BED_RMS_DB:.0f} dBFS RMS; "
        f"pink floor {TV_FLOOR_RMS_DB:.0f} dBFS RMS; 16 kHz mono s16le")
    return processing


def build_noise(clip, staging, out_dir):
    target = out_dir / f"{clip['id']}.bin"
    raw = staging / f"{clip['id']}-raw.bin"
    delay_ms = int(round(clip["clink_at"] * 1000))
    graph = (
        f"[0:a]lowpass=f=6000[bed];"
        f"[1:a]volume={clip['hum_db']:.1f}dB,lowpass=f=220[hum];"
        f"[2:a]volume=0.4,afade=t=out:st=0.01:d=0.11,adelay={delay_ms}|{delay_ms}[clink];"
        f"[bed][hum][clink]amix=inputs=3:normalize=0:duration=first,"
        f"afade=t=in:st=0:d=0.03,afade=t=out:st={max(clip['seconds'] - 0.08, 0.0):.3f}:d=0.08[a]")
    ffmpeg(["-f", "lavfi", "-i", f"anoisesrc=color=pink:sample_rate={SAMPLE_RATE}:amplitude=0.16:"
                                 f"duration={clip['seconds']:.3f}:seed={clip['seed']}",
            "-f", "lavfi", "-i", f"sine=frequency={clip['hum_hz']:.1f}:duration={clip['seconds']:.3f}",
            "-f", "lavfi", "-i", f"sine=frequency={clip['clink_hz']:.1f}:duration=0.12",
            "-filter_complex", graph, "-map", "[a]", "-ar", str(SAMPLE_RATE), "-ac", "1",
            "-f", "s16le", str(raw)])
    gain = level_gain(raw, clip["target_rms_db"])
    extract(raw, 0.0, pcm_seconds(raw), target, gain_db=gain)
    raw.unlink()
    processing = (
        f"pink bed, {clip['hum_hz']:.0f} Hz hum at {clip['hum_db']:.0f} dB, "
        f"clink at {clip['clink_at']:.2f}s; level-normalised to "
        f"{clip['target_rms_db']:.1f} dBFS RMS target; 16 kHz mono s16le")
    return processing


def manifest_url(clip):
    if clip["source"] == LIBRISPEECH_SOURCE:
        return LIBRISPEECH_URL
    if clip["source"] == COMMON_VOICE_SOURCE:
        return COMMON_VOICE_URL
    return ""


def manifest_license(clip):
    if clip["source"] == LIBRISPEECH_SOURCE:
        return LIBRISPEECH_LICENSE
    if clip["source"] == COMMON_VOICE_SOURCE:
        return COMMON_VOICE_LICENSE
    return "synthetic"


def main(argv):
    parser = argparse.ArgumentParser(
        prog="direct-speech-fixtures.py",
        description="Build the labelled direct-speech fixture corpus for the voice service.",
        epilog="Raw downloads stay under --cache; only the clips, manifest and licences are written to --out.")
    parser.add_argument("--out", default=str(OUT_DEFAULT))
    parser.add_argument("--cache", default=str(CACHE_DEFAULT))
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--skip-fetch", action="store_true")
    options = parser.parse_args(argv)
    out_root = pathlib.Path(options.out)
    cache_root = pathlib.Path(options.cache)
    if options.only and out_root.resolve() == OUT_DEFAULT.resolve():
        raise SystemExit("--only would write a manifest describing only the selected classes; "
                         "rebuild the whole corpus, or pass --out with a scratch directory")
    staging = cache_root / "staging"
    staging.mkdir(parents=True, exist_ok=True)
    clips = [clip for clip in CLIPS if not options.only or clip["class"] in options.only]
    if not options.skip_fetch:
        wanted = {clip["file"] for clip in clips if "file" in clip}
        wanted.update(turn[0] for clip in clips if "turns" in clip for turn in clip["turns"])
        es_names = sorted(name for name in wanted if name.startswith("common_voice_"))
        en_names = sorted(name for name in wanted if not name.startswith("common_voice_"))
        fetch_common_voice(cache_root / "common-voice-es", es_names, staging)
        fetch_librispeech(cache_root / "librispeech", en_names)
    manifest = []
    for clip in clips:
        out_dir = out_root / clip["class"]
        out_dir.mkdir(parents=True, exist_ok=True)
        if clip["class"] == "background-noise":
            processing = build_noise(clip, staging, out_dir)
        elif clip["class"] == "side-talk":
            processing = build_side_talk(clip, cache_root, staging, out_dir)
        else:
            source = decode_source(clip["source"], clip["file"], cache_root, staging)
            if clip["class"] == "background-tv":
                processing = build_tv(clip, source, cache_root, staging, out_dir)
            else:
                processing = build_direct(clip, source, staging, out_dir)
        target = out_dir / f"{clip['id']}.bin"
        peak_db, rms_db = level(target)
        manifest.append({
            "id": clip["id"],
            "class": clip["class"],
            "lang": clip["lang"],
            "seconds": round(pcm_seconds(target), 3),
            "source": clip["source"],
            "url": manifest_url(clip),
            "license": manifest_license(clip),
            "speaker": clip["speaker"],
            "processing": f"{processing}; measured {peak_db:.2f} dBFS peak, {rms_db:.2f} dBFS RMS",
            "sha256": digest(target),
        })
    described = {f"{row['class']}/{row['id']}.bin" for row in manifest}
    on_disk = {str(path.relative_to(out_root)) for path in out_root.glob("*/*.bin")}
    stale = sorted(on_disk - described)
    if stale:
        raise SystemExit(f"{len(stale)} clips under {out_root} are not described by the manifest "
                         f"({', '.join(stale[:3])}); remove them or rebuild the whole corpus")
    manifest.sort(key=lambda row: (CLASSES.index(row["class"]), row["id"]))
    document = {
        "version": 1,
        "ffmpeg": ffmpeg_version(),
        "sampleRate": SAMPLE_RATE,
        "channels": 1,
        "clips": manifest,
    }
    (out_root / "manifest.json").write_text(json.dumps(document, indent=2) + "\n")
    total = sum(row["seconds"] for row in manifest)
    print(f"wrote {len(manifest)} clips, {total:.1f}s, "
          f"{sum((out_root / row['class'] / (row['id'] + '.bin')).stat().st_size for row in manifest)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
