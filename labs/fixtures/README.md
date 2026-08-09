# Fixtures de audio de la cámara

Base de la calibración del VAD y del control de eco. Se versionan porque son
la única forma de reproducir la calibración sin la cámara delante.

## Cómo se graban (requiere la cámara encendida)

Desde `backend/`, con el binario Release:

```bash
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-quiet.wav
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-speech.wav
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-echo.wav
```

Cada uno graba 8 s del micrófono de la cámara a 16 kHz mono (16-bit PCM).

- `camera-quiet.wav` — la habitación en silencio (sin hablar, sin movimiento).
- `camera-speech.wav` — tres frases con pausas naturales entre ellas
  (p. ej. "Hola Argus" / "¿Qué hora es?" / "Gracias").
- `camera-echo.wav` — una respuesta de Argus sonando por el altavoz de la
  cámara, grabada desde el propio micrófono de la cámara (sin voz humana).

## Cómo se calibra

```bash
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-quiet.wav
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-speech.wav
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-echo.wav
```

Criterio de aceptación:

- `camera-quiet`: **0 turnos**. Si sale alguno, subir `vad.min_mean_prob` en
  `config.toml` hasta que no salga.
- `camera-speech`: **exactamente 3 turnos**, uno por frase. Más turnos =
  `vad.min_silence_frames` demasiado bajo (frases partidas); menos =
  demasiado alto (frases fusionadas).
- `camera-echo`: buscar el instante en que la probabilidad de voz vuelve por
  debajo de `vad.neg_threshold` tras el final del audio enviado; ese retardo
  redondeado hacia arriba es `tapo.talk_drain_margin_ms`.

Anotar los números obtenidos en `CONTEXT.md`.
