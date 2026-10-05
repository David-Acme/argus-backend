# Face test fixtures

Six face crops used by `identity-face-model-test` to pin the face pipeline
end to end (detection, alignment, the recognizer and the face-model upgrade).

They are cropped from official NASA photographs, which are works of the
United States Government and in the **public domain** (NASA media usage
guidelines: https://www.nasa.gov/nasa-brand-center/images-and-media/). They
are cropped around the face and re-encoded as JPEG (quality 90), nothing else.
Their use here implies no endorsement by NASA or by the people shown.

| File | Person | Source (images.nasa.gov) |
|------|--------|--------------------------|
| `barratt-a.jpg` | Michael Barratt | `jsc2023e047424_alt` (Crew-8 portrait) |
| `barratt-b.jpg` | Michael Barratt | `jsc2023e070781` (Crew-8 portrait, another session) |
| `meir-a.jpg` | Jessica Meir | `jsc2025e078605_alt` (spacesuit portrait) |
| `meir-b.jpg` | Jessica Meir | `jsc2025e078652_alt` (black and white portrait) |
| `hathaway.jpg` | Jack Hathaway | `jsc2025e068207_alt` |
| `menon.jpg` | Anil Menon | `jsc2026e000701_alt` |

With the shipped model the two pairs score 0.90 (Barratt) and 0.71 (Meir,
colour against black and white) and every cross pair stays at or below
0.15, so the suite pins behaviour, not accuracy. The accuracy numbers live in
`services/identity/CONTEXT.md`.
