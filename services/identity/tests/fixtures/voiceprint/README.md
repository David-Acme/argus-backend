# Voiceprint test fixtures

Nine 3-second clips of read English speech, used by
`identity-voiceprint-test` to drive enrollment, verification and
identification end to end. Each file is raw mono PCM, signed 16-bit
little-endian, 16 kHz, 48 000 samples (no header) — `.bin`, so the repository
gates treat it as data.

They are cut from **Mini LibriSpeech** (`dev-clean-2`, OpenSLR resource 31),
a subset of LibriSpeech: V. Panayotov, G. Chen, D. Povey, S. Khudanpur,
"LibriSpeech: an ASR corpus based on public domain audio books", ICASSP 2015.
LibriSpeech is distributed under **CC BY 4.0**
(https://creativecommons.org/licenses/by/4.0/); the clips are trimmed
(0.25 s into each utterance, 3 s long) and converted to 16-bit PCM, nothing
else.

| File | Speaker | Source utterance |
|------|---------|------------------|
| `alpha-1.bin` … `alpha-4.bin` | 1993 | `1993-147964-0000` … `0003` |
| `bravo-1.bin`, `bravo-2.bin` | 2803 | `2803-154320-0000`, `0001` |
| `bravo-3.bin`, `bravo-4.bin` | 2803 | `2803-154320-0003`, `0004` |
| `charlie-1.bin` | 1988 | `1988-147956-0000` |

The two enrolled speakers were chosen for a wide margin with the default
model (same speaker ≈ 0.84 against the three-clip centroid, different
speakers ≤ 0.01), so the suite pins behaviour, not accuracy. The accuracy
numbers live in `services/identity/CONTEXT.md`.
