#!/usr/bin/env python3
import argparse
import csv
import hashlib
import io
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "models" / "stt-eval"

COMMON_VOICE_REVISION = "main"
COMMON_VOICE_BASE = "https://huggingface.co/datasets/fsicoli/common_voice_17_0/resolve/" + COMMON_VOICE_REVISION
COMMON_VOICE_TSV = COMMON_VOICE_BASE + "/transcript/es/test.tsv"
COMMON_VOICE_TAR = COMMON_VOICE_BASE + "/audio/es/test/es_test_0.tar"
COMMON_VOICE_LICENSE = "CC0-1.0"
COMMON_VOICE_PER_ACCENT = 8
COMMON_VOICE_UNLABELLED = 40
COMMON_VOICE_MAX_ACCENTS = 14
COMMON_VOICE_MAX_BYTES = 900_000_000

SLR73_BASE = "https://www.openslr.org/resources/73"
SLR73_LICENSE = "CC-BY-SA-4.0"
SLR73_PER_VOICE = 40
SLR73_VOICES = ("female", "male")

MAX_WORDS = 25
MIN_WORDS = 3

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def order_key(text):
    return hashlib.sha1(text.encode()).hexdigest()


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "argus-stt-eval"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def to_pcm(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-y", "-i", str(source), "-f", "s16le",
         "-ac", "1", "-ar", "16000", str(target)],
        check=True)


def convert_bytes(data, suffix, target):
    staging = target.with_suffix(suffix)
    staging.parent.mkdir(parents=True, exist_ok=True)
    staging.write_bytes(data)
    try:
        to_pcm(staging, target)
    finally:
        staging.unlink(missing_ok=True)


def select_common_voice(rows):
    chosen = []
    by_accent = {}
    unlabelled = []
    for row in sorted(rows, key=lambda r: order_key(r["path"])):
        words = row["sentence"].split()
        if not MIN_WORDS <= len(words) <= MAX_WORDS:
            continue
        accent = row.get("accents", "").strip()
        if accent:
            by_accent.setdefault(accent, []).append(row)
        else:
            unlabelled.append(row)
    ranked = sorted(by_accent.items(), key=lambda item: -len(item[1]))
    for accent, group in ranked[:COMMON_VOICE_MAX_ACCENTS]:
        chosen.extend(group[:COMMON_VOICE_PER_ACCENT])
    chosen.extend(unlabelled[:COMMON_VOICE_UNLABELLED])
    return chosen


def common_voice(out, manifest):
    text = fetch(COMMON_VOICE_TSV).decode("utf-8")
    rows = list(csv.DictReader(io.StringIO(text), delimiter="\t", quoting=csv.QUOTE_NONE))
    chosen = select_common_voice(rows)
    wanted = {row["path"]: row for row in chosen}
    target_dir = out / "common-voice-es"
    pending = {name for name in wanted if not (target_dir / (Path(name).stem + ".pcm")).exists()}
    request = urllib.request.Request(COMMON_VOICE_TAR, headers={"User-Agent": "argus-stt-eval"})
    seen_bytes = 0
    if pending:
        with urllib.request.urlopen(request, timeout=300) as response:
            with tarfile.open(fileobj=response, mode="r|") as archive:
                for member in archive:
                    seen_bytes += member.size
                    name = Path(member.name).name
                    if name in pending:
                        data = archive.extractfile(member).read()
                        convert_bytes(data, Path(name).suffix, target_dir / (Path(name).stem + ".pcm"))
                        pending.discard(name)
                    if not pending or seen_bytes > COMMON_VOICE_MAX_BYTES:
                        break
    for name, row in wanted.items():
        pcm = target_dir / (Path(name).stem + ".pcm")
        if not pcm.exists():
            continue
        accent = row.get("accents", "").strip() or "unlabelled"
        manifest.append({
            "set": "common-voice-es",
            "id": Path(name).stem,
            "pcm": str(pcm.relative_to(out)),
            "reference": row["sentence"],
            "meta": f"accent={accent};gender={row.get('gender', '')}",
            "license": COMMON_VOICE_LICENSE,
            "sha256": digest(pcm),
        })


class RangeFile(io.RawIOBase):
    def __init__(self, url):
        self.url = url
        request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "argus-stt-eval"})
        with urllib.request.urlopen(request, timeout=60) as response:
            self.size = int(response.headers["Content-Length"])
        self.position = 0

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.position

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.position = offset
        elif whence == io.SEEK_CUR:
            self.position += offset
        else:
            self.position = self.size + offset
        return self.position

    def readinto(self, buffer):
        want = len(buffer)
        if want == 0 or self.position >= self.size:
            return 0
        end = min(self.position + want, self.size) - 1
        request = urllib.request.Request(
            self.url, headers={"Range": f"bytes={self.position}-{end}", "User-Agent": "argus-stt-eval"})
        with urllib.request.urlopen(request, timeout=120) as response:
            data = response.read()
        buffer[:len(data)] = data
        self.position += len(data)
        return len(data)


def slr73(out, manifest):
    target_dir = out / "slr73-peruvian-es"
    for voice in SLR73_VOICES:
        index = fetch(f"{SLR73_BASE}/line_index_{voice}.tsv").decode("utf-8")
        entries = []
        for line in index.splitlines():
            parts = line.split("\t")
            if len(parts) == 2 and MIN_WORDS <= len(parts[1].split()) <= MAX_WORDS:
                entries.append((parts[0], parts[1]))
        entries.sort(key=lambda entry: order_key(entry[0]))
        entries = entries[:SLR73_PER_VOICE]
        archive = zipfile.ZipFile(RangeFile(f"{SLR73_BASE}/es_pe_{voice}.zip"))
        names = {Path(info.filename).stem: info.filename for info in archive.infolist() if info.filename.endswith(".wav")}
        for identifier, sentence in entries:
            pcm = target_dir / f"{identifier}.pcm"
            if not pcm.exists():
                if identifier not in names:
                    continue
                convert_bytes(archive.read(names[identifier]), ".wav", pcm)
            manifest.append({
                "set": "slr73-peruvian-es",
                "id": identifier,
                "pcm": str(pcm.relative_to(out)),
                "reference": sentence,
                "meta": f"accent=peruvian;gender={voice}",
                "license": SLR73_LICENSE,
                "sha256": digest(pcm),
            })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    common_voice(out, manifest)
    slr73(out, manifest)
    path = out / "manifest.tsv"
    fields = ["set", "id", "pcm", "reference", "meta", "license", "sha256"]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, delimiter="\t", quoting=csv.QUOTE_NONE,
                                escapechar="\\", lineterminator="\n")
        writer.writeheader()
        for row in manifest:
            writer.writerow(row)
    print(f"wrote {len(manifest)} clips to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
