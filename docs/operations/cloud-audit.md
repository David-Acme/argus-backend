# Auditoría completa del backend de Argus

Instrucciones para una sesión de Claude en la nube. Léelas enteras antes de empezar.

## Tu papel

Eres un auditor senior de C++20, seguridad y sistemas distribuidos. Audita **todo el backend** de este
repositorio (rama `master`) y entrega un informe. **No cambies código de producción.** Tu único
cambio permitido es añadir el informe (ver "Entrega").

Responde y escribe el informe **en español**.

## Contexto del producto

- **Argus**: plataforma 100% local de seguridad para el hogar y asistente de voz. Todo corre en el
  hardware del usuario (reconocimiento facial, LLM, visión, STT/TTS). Pre-beta.
- **Mercado**: Perú. Ley 29733 de protección de datos personales y su reglamento (DS 016-2024-JUS);
  la directiva de videovigilancia (imágenes 30 días, máximo 60, 120 para incidentes). Los datos
  biométricos (cara, voz) son datos sensibles.
- **Stack**: C++20, Drogon (HTTP/WebSocket), SQLite con `DbClient` asíncrono (sin ORM), gRPC entre
  microservicios, NATS JetStream, LiveKit (WebRTC) para voz, go2rtc para cámaras (WebSocket fMP4 y
  WebRTC), llama.cpp, sherpa-onnx, ncnn, sqlite-vec.
- **Reglas del proyecto**: `AGENTS.md` en la raíz (léelo primero; son obligatorias) y el
  `CONTEXT.md` de cada servicio que revises.

## Mapa del código

- `services/*`: microservicios. `auth`, `identity`, `camera`, `guard`, `notification`,
  `productivity`, `settings`, `sync`, `voice`, `llm`, `stt`, `tts`, `vlm`, `tunnel`.
- `packages/lib/*`: infraestructura (auth, http, errors, validation, runtime, sqlite, storage...).
- `packages/contracts/*`: tipos y `.proto` que cruzan servicios.
- `packages/clients/*`: SDKs gRPC/HTTP entre servicios.
- `scripts/`: gates (`check-comments.sh`, `check-deps.sh`, `check-routes.sh`, `check-tidy.sh`),
  despliegue (`setup.sh`, `provision-host.sh`), fixtures dorados (`scripts/fixtures/http`).
- `argus-deploy/`: plantilla de docker compose y configs de ejemplo.

## Qué auditar, por prioridad

### 1. Seguridad

1. **Autenticación y sesiones** (`services/auth`, `packages/lib/auth`)
   - Cadena `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter` en toda ruta protegida;
     contrasta con `scripts/lib/route-baseline.txt`.
   - Rotación del refresh token: un solo uso, protección contra replay, atomicidad, binding de
     dispositivo. Revocación de sesiones, `ACCOUNT_DISABLED`, último Owner protegido.
   - Origen de la petición: `X-Forwarded-For`, proxies de confianza, clasificación LAN/túnel.
     ¿Se puede falsear para parecer "en casa"?
   - JWT: secretos duales, `iss`, expiración, `jti`, algoritmo fijado.
2. **Autorización** (`packages/lib/auth/src/auth/role-access.hxx`): rutas con más permiso del que
   describe `AGENTS.md` regla 7; fugas de datos de otros usuarios a Resident/Guest por HTTP o por
   `/sync`.
3. **Biometría** (`services/identity`): alineación y umbrales del reconocimiento facial, migración de
   embeddings, huella de voz pasiva, suplantación, que el login no dependa de señales débiles.
4. **WebRTC**: `POST /rtc/token` (`services/sync`) y `POST /camera/{id}/webrtc` (`services/camera`).
   Alcance y caducidad de tokens, reescritura del SDP, que go2rtc y LiveKit no queden expuestos,
   corte de espectadores al revocar una sesión o retirar el consentimiento de audio.
5. **Respuesta a intrusos** (`services/guard`, `services/notification`): botón de pánico, PIN de
   coacción (hash lento y salado, respuesta indistinguible, bloqueo por intentos), que quien da la
   alarma no la reciba, que nada de esto aparezca en listas, logs o sync.
6. **Privacidad y consentimiento**: sin consentimiento no se procesa ni se guarda (cara en cámaras,
   voz, presencia, audio de cámara, visitantes); borrado real al retirarlo; retención; capacidades
   de un solo uso para retratos y recortes de cara.
7. **Secretos**: ningún secreto, token, hash de invitación, credencial de cámara, URL firmada o IP
   real en logs, respuestas, fixtures, configs de ejemplo o en el historial del repo.
8. **Entradas**: validación de DTOs, SQL siempre parametrizado, path traversal, límites de tamaño de
   imagen/audio/SDP, parsing de JSON, multipart y protobuf, SSRF en sondeo de cámaras.
9. **Superficie de red y despliegue**: puertos expuestos en `argus-deploy/docker-compose.yml`, TLS,
   mDNS, túnel, contenedores con privilegios, permisos de ficheros (0600) en `setup.sh` y
   `provision-host.sh`.

### 2. Corrección y concurrencia

- Coroutines de Drogon: referencias capturadas en lambdas que suspenden, temporales pasados a
  coroutines, trabajo síncrono o bloqueante en el hilo del event loop o en el hilo de la conexión
  SQLite (el commit `09f5c331` de `services/camera` muestra el patrón peligroso).
- `BlockingTask`: ningún trabajo puede esperar a otro de su misma lane (deadlock).
- Transacciones: escrituras que deben ser atómicas (invitaciones, rotación de tokens, consumo de
  capacidades, outbox, fan-out de auditoría).
- Sync (`services/sync`): paginación por cursor, filas solo de creación más diffs de auditoría, que
  nada se pierda ni se duplique al reconectar.
- Cierre ordenado (drains), reinicios, reconexión a NATS, comportamiento cuando un servicio vecino
  está caído o arrancando.

### 3. Rendimiento

- Consultas N+1, índices que faltan en cursores y joins, sentencias por fila dentro de bucles.
- Copias y asignaciones en caminos calientes: audio, vídeo, inferencia, fan-out.
- Tamaño de pools de hilos (`ThreadBudget`), memoria de modelos, latencia de voz.

### 4. Arquitectura y mantenibilidad (`AGENTS.md`)

- Aislamiento de bases de datos entre servicios (regla 27) y llamadas solo por `packages/clients`.
- Tiers de dependencias (regla 25), estructura `feature`/`shared` (regla 23), structs de parámetros
  (regla 2), enums (regla 1), errores lanzados (regla 6), sin comentarios en código (regla 20).
- Código muerto, duplicado o estructura sin consumidor.

### 5. Tests

- Huecos de cobertura en lo crítico (auth, pánico/coacción, consentimiento, sync, WebRTC).
- Tests que no pueden fallar o que dependen del orden o del reloj.

## Cómo trabajar

- Lee el código; no te fíes solo de nombres o comentarios de documentación.
- Compilar todo es costoso. Si necesitas confirmar algo, prioriza la lectura y, solo si hace falta,
  compila un servicio concreto (`./scripts/build-all.sh dev --only <servicio>`).
- Cada hallazgo debe tener evidencia concreta (archivo y línea). Si no estás seguro, márcalo como
  "posible".
- No reportes estilo menor salvo que viole una regla de `AGENTS.md`.

## Entrega

1. Crea la rama `audit/cloud-<fecha>`.
2. Escribe el informe en `docs/history/reports/cloud-audit-<fecha>.md` con:
   - **Resumen ejecutivo**: los 5 riesgos más urgentes, en lenguaje claro.
   - **Tabla de hallazgos**, de más a menos grave:

     | # | Gravedad | Área | Archivo:línea | Problema | Escenario de fallo o ataque | Arreglo sugerido |
     |---|----------|------|---------------|----------|------------------------------|------------------|

     Gravedad: Crítica, Alta, Media, Baja.
   - **Mejoras de rendimiento** con el impacto esperado.
   - **Lo que está bien** (breve), para no tocarlo.
   - **Lo que no pudiste revisar** y por qué.
3. Haz commit solo de ese archivo, sube la rama y abre un pull request contra `master` titulado
   "Auditoría cloud del backend". No modifiques ningún otro archivo.
