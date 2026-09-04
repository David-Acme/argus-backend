# Argus — Plan de migración a microservicios (packages + plugins + Docker)

> Fecha: 2026-09-03 · Estado: propuesta para revisión · Enfoque: backend
>
> Este documento consolida el análisis exhaustivo del código actual (tres auditorías con
> verificación file:line) y la investigación de ecosistema (Drogon/Trantor, gRPC+Conan,
> gateways, NATS, Conan multi-repo, runtime de plugins móvil). Todo lo marcado como
> **verificado** fue confirmado en el código con `file:line`.

---

## 1. Resumen ejecutivo

Argus es hoy **un solo binario C++20** (Drogon 1.9.13 + SQLite + submodules de IA) con:

- 34 directorios de servicios bajo `src/shared/services/`, **49 rutas HTTP + 1 WebSocket
  (`/sync`)** = 50 endpoints (recontado con grep sobre `ADD_METHOD_TO`; ya sin
  `/auth/has-admin`, que se elimina — ver §2.3).
- Una única DB SQLite (`database/argus.db`) compartida por **4 conexiones dueñas**.
- **Sin librerías internas, sin CI, sin tests (`src/test/` vacío), sin versionado de API.**
- Toda la IA corre in-process vía submodules (llama.cpp, sherpa-onnx, ncnn, ONNX Runtime,
  fastText). Solo `go2rtc` es proceso externo.

El objetivo: dividirlo en **microservicios independientes** (un repo, una DB, unas dependencias
y unos labs por servicio), comunicados por **gRPC**, orquestados en **Docker/Swarm**, con una
capa de **packages** (interfaces tipadas sobre los servicios) y una de **plugins** (features
instalables del frontend sobre una base casi vacía), con **versionado y retrocompatibilidad
formales** para que la app móvil existente nunca se rompa, y con **acceso remoto E2E** por
túnel terminando TLS en el gateway (§3.7).

Estrategia: **strangler pattern** — un gateway propietario adopta el rol del binario actual y
va re-enrutando dominios a microservicios fase por fase. En cada fase la app móvil real sigue
funcionando sin actualizarse. Esa es la definición operativa de retrocompatibilidad de este plan.

---

## 2. Estado actual auditado (lo que el código obliga a respetar)

### 2.1 El binario y su ciclo de vida

- Un solo target: `add_executable(argus-backend src/main.cc)` (`CMakeLists.txt:58-61`), fuentes
  por `file(GLOB_RECURSE)` en 4 grupos (`CMakeLists.txt:42-56`). **No existe ninguna
  `add_library` interna.**
- `Application::run()` (`src/config/application.cc:115-200`): ConfigService::load →
  `app().loadConfigJson(ConfigService::drogonConfig())` → CORS/OPTIONS → handlers de error →
  señales → `DbService::migrate(5)` + pragmas + `sqlite3_auto_extension(sqlite3_vec_init)` →
  banner de pairing → `registerServices()` → `llama_backend_init()` → **Go2rtcManager,
  MediaRelay y StreamHub se inicializan FUERA del ServiceRegistry** (`:186-188`) → `app().run()`.
  Shutdown en orden inverso (`:202-209`).
- TLS lo termina Drogon: listener definido **en `config.toml`**, no en código
  (`config.toml:3-9`), con `certs/server.pem` (cadena completa: CA de instancia + leaf, EC
  P-256, SANs `argus.local, localhost, 127.0.0.1, <mdns.name>.local`, generados por
  `setup.sh:297-358` y rotados por `CertService`).

### 2.2 El ServiceRegistry es decorativo (hoy)

- Contrato `IService` (`src/config/service.hxx:7-20`): name/version/dependencies/initialize/
  isLoaded/shutdown/health.
- `ServiceRegistry::initialize()` hace orden topológico Kahn (`service-registry.cc:12-74`)…
  **pero ningún adapter sobreescribe `dependencies()`** (grep lo confirma): el sort degenera en
  orden de registro y el orden real lo fija `Application::registerServices()`
  (`application.cc:211-225`): room_manager, cert, mdns, tts, llm, stt, vision, face, intent,
  memory, queue, extract.
- `ServiceRegistry::health()` y `names()` tienen **cero llamadores**: no existe ruta `/health`.

### 2.3 La superficie HTTP (49 rutas + 1 WS, paths planos, sin versión)

- Registradas por macros `HttpController<T>` **dentro del `.hxx` de cada controller**
  (auto-registro por static-init al linkarse el TU). No hay tabla central de rutas.
- Cadena de filtros canónica: `DeviceFilter → ValidJsonFilter → JwtFilter → RoleFilter`
  (`AGENTS.md:110-118`), referenciados **por nombre en string** en cada controller.
  - `DeviceFilter` (`src/filter/device/device-filter.cc`): fingerprint HMAC-SHA256 de
    `User-Agent|IP` con `device.fingerprint_secret`. Honra `X-Forwarded-For` **solo** si
    `device.trust_forwarded_for=true` y el peer es loopback o está en
    `device.trusted_proxy_ips` — **el bloqueador #1 para cualquier proxy intermedio**.
  - `JwtFilter` (`jwt-filter.cc`): token por `Authorization: Bearer`, **`?token=`** o cookie;
    HS256 con secret dual (`jwt.secret`/`jwt.refresh_secret`); **2 queries DB por request**
    (`user` + `refresh_token`, el access token vive en la tabla) + equality de `device_hash`.
  - `RoleFilter` → `role-access.hxx`: `kTableAccess` (`:31-70`) + `tableFromPath`
    (`:139-160`) — **un segundo mapa path→tabla que debe coincidir con las rutas**;
    `/auth/*` usa `kAuthAccess` especial (`:72-78`); el mismo archivo gobierna HTTP y sync.
- Inventario por feature (todas las rutas son top-level, sin `/api` ni `/v1`):
  - **auth** (9 rutas): `/auth/login` (multipart con imagen → **login facial**),
    `/auth/register` (multipart; bootstrap del primer owner o invitación; transacción
    IMMEDIATE user+person+face_embedding+invitation_redemption + HNSW en-proceso),
    `/auth/status`, device-login (QR, TTL 120 s), `/auth/refresh-token`
    (single-use, replay protection), `/auth/logout` (invalida + desconecta sockets WS del
    usuario), `/auth/me`. **`/auth/has-admin` se elimina** (decisión del usuario: el frontend
    nunca la consume — el frontend arranca por el certificado, no por esta ruta; el
    handler de controlador y `AuthService::hasAdmin` desaparecen con ella — el bootstrap de
    `/auth/register` ya hace su propio chequeo de owner dentro de la transacción).
    Segundo cambio de wire deliberado de la Phase 0: `tableNameFromString("memory")` ahora
    mapea a `TableName::Memory` (antes caía en el fallback `TableName::User`) — corrección de
    un input previamente degenerado, sancionada en el migration ledger.
  - **camera** (3): `POST/PATCH/DELETE /camera[/{id}]` — **no existe `GET /camera`**: la lista
    se lee por el WS `/sync`.
  - **camera-control** (7): `/camera/{id}/status|presets|ptz|preset|settings|capabilities|talk`.
    Error envelope propio: 404 "Camera not found" vs **502 `CAMERA_UNREACHABLE`** — el gateway
    debe preservar esa distinción. `/talk` toca `TtsService` in-proc.
  - **calendar-event / calendar-event-share / project / project-member / project-task** (3+3+3+3+3).
  - **invitation** (4; `POST /invitation/resolve` deliberadamente sin auth — onboarding).
  - **notification** (2: `/notification/read`, `/notification-token`).
  - **pairing** (1: `POST /pairing` sin auth — bootstrap de confianza PKI; devuelve
    `caFingerprint`, `serverFingerprint`, `caPem`; 409 si ya paired).
  - **user** (5: `GET /user` es **la única lista HTTP de toda la app**; portrait-preview +2).
  - **zone** (3, valida cameraId contra `camera` → 404).
- **Sin controladores HTTP para**: `event`, `person`, `reminder*`, `context_note`,
  `camera_stream`, `audit_log*`, `memory_*` — esas tablas **solo se leen por el WS `/sync`**.
- **Rutas pre-auth** (sin JwtFilter — el gateway debe replicarlas tal cual): `/auth/login`,
  `/auth/register`, `/auth/device-login` (crear y poll), `/auth/refresh-token`
  (Device+ValidJson — refresco pre-autenticación), `/invitation/resolve` y `/pairing` (solo
  ValidJsonFilter). Toda ruta GET se ahorra `ValidJsonFilter` (no hay cuerpo). **Regla de
  seguridad (decisión del usuario): `/pairing` y `/auth/register` (creación de usuarios) se
  ejecutan ÚNICA y exclusivamente dentro de la red local del hogar** — el túnel (§3.7) es solo
  para dispositivos ya autenticados y registrados. El inventario completo ruta por ruta (método,
  path, filtros, handler) está en **Appendix A**.

### 2.4 El WebSocket `/sync` (el contrato más load-bearing)

- Única ruta WS: `WS_PATH_ADD("/sync", "DeviceFilter", "JwtFilter")` (`sync-socket.hxx:18`),
  frames texto cap 64 KiB, binarios van a la sesión de voz.
- Al conectar: rooms numéricas por tabla + room de usuario
  (`moduleRoom = 1+(uint8_t)table`, `userRoom = 1000+userId`, `room-manager.hxx:11-19`) y envía
  `InitialInfo`.
- Mensajes cliente→servidor (`sync-service.cc:98-205`): `sync` (bootstrap de 15 tablas),
  `sync_audit_log` / `sync_user_audit_log` (diffs, cursor por **id**),
  `camera:subscribe` (`{cameraId, quality}` → **binarios fMP4 por el mismo socket**,
  `mime:"video/mp4"`, window 128 KiB, cap de subs → 429), `camera:ack` (flow control),
  `camera:unsubscribe`, `voice:start|stop|skip` (→ **binarios PCM int16 de TTS** por el mismo
  socket). Error envelope: `{type:"<type>_error", status, error}`.
- **Protocolo de sincronización** (`src/shared/contracts/`): `SyncOperation` **numérico**
  (0=initial_info, 1=sync, 2=sync_audit_log, 3=sync_user_audit_log, 4=add, 5=delete, 6=log,
  7=auth_context_changed); `Syncable` (find/findDeleted/findLast/findLastDeleted); cursor
  `created_at`+rowid (**`updated_at` nunca es cursor** por diseño — `AGENTS.md:479-486`),
  deletes por cursor `deleted_at`, cap `SYNC_LIMIT=200`.
- **Cada write empuja**: `SocketService.emitModule/emitUser/disconnectUser` +
  `SyncAuditService` → `AuditLogService`/`UserAuditLogService` (diff JSON plano, compactación
  diaria). **Esta es la razón por la que la división necesita un bus de eventos.**

### 2.5 La base de datos (hoy: un archivo, 4 dueños)

| File | Handles |
|---|---|
| `database/argus.db` | Pool Drogon (`number_of_connections=1`) + **VecDb** (vec0: `memory_vec`, `face_vec`) + **SqliteGraph** (grafo de memoria, FTS5) + **JobRepository** (cola durable) — todos apuntando al **mismo archivo** |

- Un único `database/schema.sql` (759 líneas) es la única fuente DDL, ejecutada
  **incondicionalmente** por las 4 conexiones al abrir (`schema-runner.hxx:9-13`).
- Versionado: tabla `schema_version` (`schema.sql:296-299`) + `DbService::migrate(target)`
  (`db-service.cc:154-240`) con migraciones **inline en C++** (V1/V2; target 5 con V3-V5 no-op)
  + column patches por introspección (`pragma_table_info`). Sin `PRAGMA user_version`, sin
  directorio de migraciones.
- Pragmas por arranque: WAL, synchronous=NORMAL, busy_timeout=5000, foreign_keys=**ON**, etc.
- **`user(id)` es un hub FK**: ~20 tablas le apuntan (user_invitation, invitation_redemption,
  stored_file, user_portrait, portrait_*, person, reminder*, project*, calendar*,
  refresh_token, audit_log, user_audit_log, notification*, user_action_log). **`person`** es el
  segundo hub (face_embedding, person_event, voice_session, voice_message, memory_entity sin
  FK). `camera` es el hub del dominio cámara (camera_stream, zone).
- **Cross-dominio por SQL directo (el peor caso)**: `memory-graph-query.hxx:171-179` lee
  `person`, `camera`, `zone`, `camera_stream` para construir un gazetteer de resolución.
- `audit_log`/`user_audit_log` son el **sustrato de sync de todos los dominios** y referencian
  cualquier tabla por nombre (`record_id`+`table_name` TEXT) — no pueden vivir en una DB de un
  solo servicio sin romper los diffs del resto.
- Tablas huérfanas (sin código que las use): `voice_session`, `voice_message` (ni repositorio),
  `context_note`, `portrait_access_request`, `portrait_access_grant`; `event` (nunca escrita,
  solo lectura sync) y `camera_stream` (solo sync).

### 2.6 Infraestructura transversal

| Primitiva | Dónde | Consumidores |
|---|---|---|
| `ConfigService` | `config-service/` (static, file-scope `gConfig`+`gOverlay`, write-back quirúrgico a `config.toml`, sin hot-reload) | **27 archivos** |
| `ThreadBudget` + `HardwareProbe` | `src/shared/wrapper/` | 8 servicios de IA (tts, stt, llm, vision, face, extract, embedding, job-queue) |
| `BlockingTask<T>` | `wrapper/blocking-task/` | puente síncrono↔coroutine en 6 servicios |
| `ai_init::llamaMutex()` | `wrapper/ai-init/` | **llm y vision comparten la llave global de llama.cpp** |
| `ToolRegistry`/`ToolExecutor` | `services/tools/` | memory (registra), LfmAdapter (llm) |
| go2rtc | `stream/go2rtc-manager.cc`: fork+setsid+execv, config generado **chmod 600 con credenciales**, supervisor con backoff 250ms→30s, máx 8 restarts; API `127.0.0.1:1984`, RTSP `8554`; `isSafeUrl()` restringe a RFC1918/loopback y rtsp/tapo/http | stream service |

### 2.7 Build, modelos, despliegue

- Conan (`conanfile.txt`): drogon/1.9.13, jwt-cpp, nlohmann_json, opencv/4.13.0 (sin CUDA),
  onnxruntime/1.24.4 (sin CUDA), tomlplusplus, mdns, qr-code-generator. **Los motores de IA NO
  vienen de Conan**: submodules (ncnn Vulkan+AVX2, llama.cpp CUDA>Vulkan>CPU, sherpa-onnx,
  sqlite-vec, fastText, inspireface-presente-pero-sin-usar, kuzu).
- Presets `dev`/`prod` (Ninja, `-j 8`), `-O3 -march=native -flto` solo Release, RPATH `$ORIGIN`,
  POST_BUILD que copia `config.toml` y symlinka `models/`, `database/`, `certs/`,
  `third_party/go2rtc/` — así resuelven las rutas runtime.
- **Modelos: 13 GB** en `models/` (todo git-ignorado): llm ~696MB (LFM2.5-1.2B Q4_0), vlm ~568MB
  (LFM2.5-VL-450M Q8 + mmproj), tts ~415MB (supertonic-3), stt ~107MB (nemo fast-conformer
  int8), extract ~469MB (NuExtract-1.5-tiny), memory ~130MB (e5-small int8), face ~5MB
  (RetinaFace+MobileFaceNet), vad 2.3MB (silero), intent ~5MB (fastText **entrenado localmente**
  por `labs/intent-probe`, no descargado).
- `setup.sh` (1088 líneas): system deps → conan → submodules → certs → modelos (con sha256 y
  `.part`+rename atómico) → build. Incluye `detect-hardware.sh` (escribe `scripts/.hw-profile`,
  tier CUDA > Vulkan > CPU), `setup_certs` (CA + leaf + `certs/pairing.code` = primeros 8 hex
  del SHA-256 de la CA), RustFS en Docker (S3, `127.0.0.1:9000`).
- Docker actual: solo RustFS en compose + un Dockerfile multi-stage del binario
  (`-DARGUS_BUILD_LABS=OFF`, non-root, EXPOSE 7024, conan==2.21.0).
- **Sin CI, sin tests** (`src/test/` vacío, `enable_testing()` ausente). La única verificación
  hoy es build limpio + labs manuales.

### 2.8 Labs (verificado hoy)

- **16 labs** en `labs/`, de los cuales **14 enlazan fuentes de `src/shared/services/`
  directamente** (compilan los `.cc` de los servicios, porque no existe ninguna librería interna
  a la que apuntar):
  - `voice-test` arrastra **42 fuentes de 16 directorios de servicio** (vad, stt, tts, llm,
    memory, intent, extract, embedding, conversation, reaction, tools, stream, tapo, sqlite,
    config, vision) — el peor caso de acoplamiento por fuentes.
  - `memory-probe` 22, `camera-control` 20, `tapo-probe` 15, `tool-bench` 7,
    `tts-probe`/`stream-probe`/`extract-probe` 6 cada uno, y el resto ≤4.
- `labs/intent-probe` **entrena localmente** el modelo fastText de intent (no se descarga:
  `models/intent/` no viene de `setup.sh`).
- Consecuencia para la migración: tras la división, cada repo tiene **SUS labs** y consume los
  demás servicios por gRPC o por el package Conan de `argus-common` — nunca compilando fuentes
  ajenas. `voice-test` se convierte en el lab de voz de base (orquestador) y deja de compilar
  tts/stt/llm/vision in-proc.

---

## 3. Decisiones de ecosistema (investigadas y justificadas)

### 3.1 Comunicación: gRPC + Protobuf vía Conan

- `grpc/1.70+` + `protobuf/>=5.27 <7` (receta oficial de Conan Center), abseil/re2/c-ares
  transitivos. C++17 mínimo requerido por grpc (ya usamos C++20).
- Codegen moderno: `find_package(gRPC CONFIG)` + `find_package(protobuf CONFIG)`;
  `protobuf_generate` ×2 — una para `*.pb.cc/h` y otra con `LANGUAGE grpc` +
  `PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>` → `.grpc.pb.cc/h`; generar a un
  **OBJECT library** con `SKIP_UNITY_BUILD_INCLUSION`; versiones protoc/runtime **pinneadas
  EXACT** (desajuste protoc↔libprotobuf es un fallo runtime silencioso clásico).
- **Pitfall conocido (Conan #19790)**: con el generador `CMakeConfigDeps` y `-Wl,-z,defs`,
  `gRPC::grpc++` solo sub-enlaza → enlazar explícitamente
  `gRPC::grpc++ gRPC::grpc gRPC::gpr protobuf::libprotobuf`.
- Reglas de producción: deadlines en todo (`set_deadline`), `IsCancelled()` en loops de
  streaming, retry con backoff en `UNAVAILABLE`/`DEADLINE_EXCEEDED`, channels/stubs reusables
  (thread-safe), keepalive, `grpc_health_v1` en todos los servicios, reflection activada para
  debugging, shutdown graceful por señal.

### 3.2 Trantor: ya es parte del stack

Trantor es la capa de red no-bloqueante (EventLoop, TcpClient/TcpServer, TLSPolicy) sobre la que
Drogon está construido — mismo autor (Tao An). **No se adopta como dependencia aparte**; es
detalle interno de Drogon. Lo que sí importa: Drogon `HttpClient` (async + coroutine
`sendRequestCoro`, pipelining, conexiones persistentes, sobre `trantor::TcpClient`) y el plugin
oficial de ejemplo **SimpleReverseProxy** (round-robin/sticky, `setPassThrough(true)` para no
mutar headers) — el benchmark del issue #307 de Drogon mostró un proxy Drogon superando a nginx
y Envoy (~50K req/s). Conclusión: el gateway en C++ con Drogon es viable **sin introducir
ninguna librería nueva**.

### 3.3 Gateway: propio con Drogon (edge + filtros preservados)

Elección: **argus-gateway** construido con Drogon usando el patrón `SimpleReverseProxy` para el
routing por prefijo de path + los filtros existentes (Device/ValidJson/Jwt/Role) ejecutándose
una sola vez en el edge.

Por qué no Traefik/Caddy/nginx como punto único de entrada:
1. **Device fingerprint**: `deviceHash = HMAC(User-Agent|IP)` — un proxy cambia el peer IP y
   (peor) puede mutar User-Agent → **401 "Device mismatch" en toda la app** salvo configurar
   `trust_forwarded_for` + proxy confiables. El gateway propio recibe al cliente directo, como
   hoy.
2. **CA de instancia propia** (no ACME): los clientes pinnean el fingerprint de la CA devuelto
   por `/pairing`; la rotación de leaf la hace `CertService` en C++. Terminar TLS fuera exige
   replicar ese ciclo de vida.
3. **mDNS** anuncia `_argus._tcp` con TXT `path=/, https=true, wss=true` — el punto de entrada
   debe seguir siendo uno solo con ese contrato.

Los proxies maduros pueden agregarse **después** como capa TLS pura si algún día se necesita
(HTTP/3, ACME de dominio público), sin tocar los servicios.

### 3.4 Bus de eventos: NATS + JetStream (cliente oficial cnats)

- **cnats** (`nats-io/nats.c`): Apache-2.0, cliente C/C++ oficial, JetStream completo desde
  v3.0 (streams, consumers durables, at-least-once con replay), **disponible en Conan**,
  v3.13.0 mantenida. TLS + NKeys.
- Alternativa evaluada: Redis Streams (menos maduro como bus dedicado), Kafka (sobredimensionado
  para LAN single-host). NATS = un binario Go, RAM mínima, ideal para edge/self-hosted.
- Sujeto a decisión en diseño: JetStream KV para config compartida (reemplazo potencial de
  `config.toml` compartido) — deferido.

### 3.5 Dependencias multi-repo: Conan 2 + registry privado

- **Principio del usuario**: cada package, plugin y proyecto principal tiene **sus propias
  dependencias** — nadie comparte un conanfile global.
- `argus-common` se **publica como package Conan** (`argus-common/1.x`) a un registry privado:
  - Arranque (1 máquina): remote `local_recipes_index` (carpeta local de recetas, sin servidor).
  - Multi-máquina: **Artifactory CE for C/C++** en Docker (gratis, WebUI, permisos, REST),
    con repos `develop → packages → products` y **promoción** entre repos (channels
    desaconsejados en Conan 2).
- Rangos de versión en los consumidores: `argus-common/[>=1.2 <2]`; `package_id` modes
  (embed/non-embed) controlan cuándo se reconstruyen binarios; inmutabilidad de binarios
  publicados.
- Submodules (ncnn, llama.cpp, sherpa-onnx…) quedan **dentro del repo del servicio que los
  usa**, no en común.

### 3.7 Acceso remoto: túnel propio, E2E y el rediseño de identidad que obliga

Requisito del usuario: todo el stack se expondrá a internet mediante un **túnel propio y
personalizado, 100% gratis** (nada pagado, nada de terceros comerciales). Su desarrollo todavía
no comienza, pero este apartado deja **el contrato definido** para cuando se construya. El túnel
tendrá tres responsabilidades:

1. **Transporte seguro** de dispositivos ya autenticados hacia `argus-gateway`, con
   encriptación **punto a punto basada en la certificado de instancia** (probablemente **gRPC**
   como protocolo del túnel).
2. **Canal de push**: el túnel es el encargado de **enviar las notificaciones push a los
   dispositivos conectados** (mantiene conexiones persistentes con ellos; ver abajo).
3. **Nada más**: el túnel es **solo para personas ya previamente autenticadas y registradas en
   el sistema**. No bootstrap: ni crear usuarios ni parear dispositivos a través del túnel.

**Regla no negociable — el TLS termina en `argus-gateway`, no en el túnel:**
- El túnel retransmite; nunca descifra. La sesión TLS del cliente va de punta a punta con los
  certificados de la instancia (la CA que `/pairing` devolvió y el cliente pinneó). Si el túnel
  usa gRPC internamente, ese gRPC es **otro canal** (túnel↔gateway) y no debe confundirse con el
  canal del cliente: dos TLS distintos, el del cliente sigue siendo E2E.
- **Dónde corre el relay (decisión del usuario): un servidor dedicado en la nube, ubicado en
  Estados Unidos** — latencia genérica alrededor del mundo — y corriendo **exclusivamente el
  túnel** (single-tenant: ningún otro servicio en la máquina). Hardware moderno (CPU potente,
  DDR5 ECC, M.2). Nota técnica: como el relay **no descifra**, su carga es puro reenvío TCP/gRPC
  — el cuello de botella real es el ancho de banda y el peering, no la CPU; el hardware dedicado
  da margen de sobra. El relay solo ve bytes cifrados.

**Lo que el túnel rompe y hay que rediseñar — la identidad de dispositivo (decisión del
usuario: la IP sale del deviceHash):**
- `DeviceFilter` hoy calcula `deviceHash = HMAC(User-Agent|IP)` (§2.3). La IP **no sirve** como
  material de identidad: cambia con la red del dispositivo (LAN del hogar, datos móviles, túnel).
  Peor aún: con el túnel, todos los remotos llegan con la IP del relay y ni siquiera existe
  `X-Forwarded-For` posible si los bytes van cifrados.
- Rediseño objetivo: **credencial de dispositivo sin IP**. El gateway emite, al login/pairing,
  una **credencial por dispositivo** (certificado de cliente mTLS emitido por `CertService` para
  el transporte, más un `device_secret` opaco que la app presenta en cada request). El hash pasa
  a `HMAC(User-Agent | credencial)` — misma identidad en LAN, en datos móviles y por el túnel;
  roaming transparente, la IP deja de existir como material de identidad.
- Transición honesta: durante la migración strangler la app no cambia (invariante), así que el
  monolito/gateway sigue calculando `HMAC(UA|IP)` en LAN. La identidad sin IP aterriza en la
  **Fase 5** junto con el túnel, coordinada vía `argus-contracts` (cambio de cliente). En ese
  momento las sesiones existentes se re-emiten una única vez (re-login controlado).

**Certificados — SANs incompletas hoy:**
- Los SANs actuales son `argus.local, localhost, 127.0.0.1, <mdns.name>.local`. El hostname
  público del relay **no está** → mismatch de TLS. `CertService` incluirá el hostname público
  en las SANs (config nueva `remote.hostname`), y la app aceptará un servidor manual además del
  descubierto por mDNS.

**Superficie expuesta — endurecimiento del edge:**
- **`/pairing` y `/auth/register` (creación de usuarios) se ejecutan ÚNICA y exclusivamente
  dentro de la red local del hogar** (decisión del usuario, motivo de seguridad: no puedes
  crear un usuario y colarte por el túnel). El gateway las rechaza cuando `remote.enabled=true`
  o cuando el peer no está en los CIDRs de LAN configurados (`network.lan_cidrs`). La
  incorporación de gente nueva es un acto físico en el hogar, como debe ser.
- El túnel solo sirve a usuarios ya registrados: la superficie pre-auth remota se reduce a
  `/auth/refresh-token` (single-use, replay protection). Aun así: **rate limiting por
  credencial** en el edge **antes** de que `JwtFilter` toque SQLite (son 2 queries por request;
  `identity.db` es de un solo escritor) y lockout progresivo.
- Solo el gateway y el componente de túnel viven en `edge`; NATS/gRPC/RustFS jamás expuestos.

**Push — quién manda qué (arquitectura del canal):**
- El dispositivo remoto mantiene **una conexión persistente con el túnel** (esa conexión es el
  push channel). El flujo de un evento (ej. YOLO26n detecta presencia): `argus-camera` publica
  a NATS → `argus-notification` decide (presupuesto, horas-silencio, dedupe, digests) → publica
  la **intención de notificación** → el túnel la consume y la entrega por las conexiones
  persistentes de los dispositivos conectados.
- Separación limpia: `argus-notification` es **política** (qué notificar, a quién, cuándo, en
  qué formato); el túnel es **transporte** (por dónde llega). Ninguno sabe del otro: se hablan
  por NATS con un contrato en `argus-contracts`.

## 4. Arquitectura objetivo

```
                         ┌──────────────────────────────────────────┐
  móvil / desktop        │        argus-gateway  (= la "base")      │
  base shell + plugins   │  TLS 7024 · misma CA de instancia        │
    │  WSS /sync         │  DeviceFilter→ValidJsonFilter→JwtFilter  │
    ▼                    │  →RoleFilter · /pairing · /auth/* · mdns │
  path routing ─────────►│  /sync: fan-out NATS + proxy media/voz   │
  argus.local/camera/…   └───────┬──────────────────────────────────┘
                                 │  gRPC (mTLS interno, deadlines, health)
        ┌──────────────┬─────────┴────┬──────────┬──────────┬─────────┐
        ▼              ▼              ▼          ▼          ▼         ▼
  argus-productivity  argus-camera  argus-llm  argus-vlm  argus-tts  argus-stt
   calendar, project,  camera, zone, llama.cpp   LFM2.5-VL  supertonic sherpa-onnx
   reminder            stream/go2rtc, +tools                (onnx)     (onnx)
                       YOLO26n,
                       operator
                        + argus-memory (grafo+extract+embedding+VecDb+FTS5)
                        + NATS JetStream · RustFS (S3)
```

### 4.1 Repositorios (monorepo único; cada servicio compila lo suyo, pero todo vive como una sola aplicación)

Reglas de composición (decisión del usuario):

- **Un único monorepo** (la raíz del repo actual del backend). Cada servicio vive como una
  carpeta `argus-*` hermana de `src/` (`argus-gateway/`, `argus-camera/`, …), con su propio
  `CMakeLists.txt`, `CMakePresets.json`, `conanfile.txt`, `database/schema.sql`, `config/` y
  `labs/` — compila independiente, produce sus propios artefactos de build y se despliega como
  contenedor propio, **pero todo junto opera como una sola aplicación** (un gateway, un
  contrato, una UI).
- **Versionado interno por tags del repo**: `contracts-v*` para `argus-contracts/` y
  `service-v*` para cada carpeta de servicio. La CI filtra por ruta (`paths`) para compilar y
  testear solo lo que cambió.
- **Cada carpeta de servicio lleva su `CONTEXT.md` y su `AGENTS.md`, ambos en inglés** (el
  "why" viaja con el código, las reglas de agente también).
- **Infra por necesidad, no por uniformidad**: `argus-camera` necesita S3 (RustFS) para sus
  artefactos; los demás solo consumen lo que su dominio pide. La infra se declara en el compose
  de `argus-deploy`, no en cada servicio.
- **Reutilización en `argus-common`** (validación de DTOs/DSL, envelope, role-access, enums,
  ConfigService…) para no redundar código que ya existe.
- **`argus-auth` (package, nuevo)**: un package simple que encapsula TODO el ciclo de
  autenticación — DeviceFilter/JwtFilter/RoleFilter, JwtService, device credential (sin IP,
  §3.7), validación y helpers — para que gateway y servicios mantengan la lógica de auth
  sencilla, escalable y mantenible en un solo lugar. Depende de `argus-common`.
- **`argus-tunnel` se contempla desde el inicio pero como CLIENTE** (decisión del usuario: el
  túnel solo está en idea). El folder `argus-contracts` define el contrato del cliente
  (conexión al relay, canal de
  push) y su integración en compose; el relay propio es implementación futura.
- **Tests separados por tipo en cada servicio**: `test/unit/`, `test/e2e/` y cobertura; e2e
  ejercita el contrato por HTTP/gRPC. Cada cosa con sus tests especializados y sus tests
  principales.
- **Convención de nombres de tests (decisión del usuario, cambia la de AGENTS.md actual
  `*_test.cc`)**: guion, no underscore — `user-service-test.cc` / `user-service-test.hxx`
  (antes `user-service_test.cc`). El resto de la declaración de archivos sigue el patrón que ya
  se trabaja (`.hxx` headers, `.cc` fuentes, kebab-case), sin variantes nuevas.

| Servicio | Contiene | Deps propias (conanfile) |
|---|---|---|
| `argus-contracts` | Todos los `.proto` (`argus.<dominio>.v1`), schemas de manifiestos package/plugin, fixtures de frames `/sync`, `buf` + CI de breaking | buf, protoc |
| `argus-common` | ConfigService, ThreadBudget, HardwareProbe, BlockingTask, CancellationToken, schema-runner, envelope `ApiResponse`, `role-access`, enums, validation, logging | toml++, nlohmann_json, jwt-cpp |
| `argus-gateway` | TLS, filtros, auth facial, pairing, certs, mdns, dueño de `/sync` (fan-out + proxy), reverse-proxy | drogon, cnats, argus-common |
| `argus-camera` | `camera`, `camera_stream`, `zone`; camera-control; drivers (tapo); go2rtc/StreamHub/MediaRelay; **YOLO26n (ObjectDetectorService + CameraOperatorService + EventIntelligence + seam IObjectDetector)** | drogon, ncnn (Vulkan), opencv, cnats |
| `argus-productivity` | `calendar_event(+share)`, `project(+member,+task)`, `reminder(+detail)`, `context_note` | drogon, cnats |
| `argus-notification` | `notification`, `notification_token`; **política** de notificación (presupuesto, horas-silencio, digests); la **entrega** física la hace el túnel vía NATS (§3.7) | drogon, cnats |
| `argus-llm` | LLM (llama.cpp in-proc) + LfmAdapter/tools + intent (fastText) | llama.cpp, fastText |
| `argus-vlm` | Visión/LFM2.5-VL (llama.cpp mtmd in-proc) | llama.cpp (mtmd) |
| `argus-tts` | supertonic-3 (onnxruntime in-proc) | onnxruntime |
| `argus-stt` | sherpa-onnx nemo-transducer in-proc | sherpa-onnx, onnxruntime |
| `argus-memory` | grafo de memoria + extract (NuExtract) + embedding (e5) + VecDb (`memory_vec`) + FTS5 | llama.cpp, onnxruntime, sqlite-vec |
| `argus-tunnel` | Túnel propio (§3.7): cliente en el hogar + relay en **servidor dedicado en la nube (EE.UU., single-tenant)**; gRPC; conexiones persistentes con dispositivos (canal de push); retransmite TLS de la instancia sin descifrar | grpc, cnats |
| `argus-deploy` | compose/swarm, Dockerfiles por servicio (incluye el relay del túnel), instalador, secrets, detect-hardware | — |

Regla de datos: **ningún servicio hace SQL contra la DB de otro**. Referencias cross-dominio →
ids validados por el servicio dueño o RPC gRPC.

### 4.2 Separación de dominios de datos (una DB por archivo)

| Servicio | Tablas que posee (del `schema.sql` actual) |
|---|---|
| **gateway (base)** | `user`, `refresh_token`, `device_login_challenge`, `user_invitation`, `invitation_redemption`, `person`, `face_embedding` (+ `face_vec`), `audit_log`, `user_audit_log`, `user_action_log`, `stored_file`, `user_portrait`, `portrait_preview_capability` |
| **camera** | `camera`, `camera_stream`, `zone` |
| **productivity** | `calendar_event`, `calendar_event_share`, `project`, `project_member`, `project_task`, `reminder`, `reminder_detail`, `context_note` |
| **notification** | `notification`, `notification_token` |
| **memory** | `memory_entity`, `memory_alias`, `memory_fact`, `memory_edge`, `memory_episode`, `memory_source`, `memory_procedure`, `memory_*_fts`, `memory_vec` |
| **gateway (sync/audit)** | `schema_version` propio por DB; sync de todas las tablas vía NATS |

Nota: `user(id)` deja de ser hub FK — las demás DBs guardan `user_id` como entero validado en el
servicio que lo usa (o vía RPC a base). `person` sigue vivo en base; `face_embedding` y
`memory_entity.person_id` lo referencian por id (memory hoy ya lo hace sin FK).

### 4.3 Identidad y seguridad

- El **gateway es el único que ve al cliente**: ejecuta DeviceFilter (HMAC User-Agent+IP intacto
  durante la migración; ver §3.7 para la evolución a identidad por certificado de cliente mTLS
  con acceso remoto) → JwtFilter (2 queries a `identity.db`) → RoleFilter (`role-access`
  intacto), y **propaga la identidad por metadata gRPC mTLS** (`x-argus-user`, `x-argus-role`,
  `x-argus-device`) a los servicios. Los servicios confían solo en la red `internal`.
- `/pairing`, certs y mdns viven en el gateway (bootstrap de confianza PKI intacto: pairing
  code = fingerprint de la CA). **Pairing y creación de usuarios solo en LAN** (§3.7); por el
  túnel solo pasan dispositivos ya autenticados.
- CORS `*` + short-circuit de OPTIONS: el gateway lo replica **exactamente igual** (o aparecen
  headers duplicados).
- Los cuerpos multipart (`/auth/login`, `/auth/register`) pasan byte-exactos.

---

## 5. Versionado y retrocompatibilidad (política formal)

| Capa | Regla |
|---|---|
| **gRPC** | Paquetes `argus.<dominio>.v1` · SemVer por paquete · cambios solo aditivos; campo retirado → `reserved` + campo nuevo · CI `buf breaking` contra la versión publicada · health-checking + reflection en todos |
| **HTTP existente** | **Paths, envelope `{status, info, errors}` (siempre presentes, `info`/`errors` null cuando vacío), códigos de error (`BAD_REQUEST…CAMERA_UNREACHABLE`) y `SyncOperation` 0–7: congelados para siempre.** El gateway enruta por prefijo de path idéntico al actual |
| **Cambios breaking futuros** | Ruta nueva versionada (`/v2/...`); el gateway puede servir v1 y v2 en paralelo durante la ventana |
| **Deprecación** | Ventana N-1 minor: header `Deprecation` + `Sunset` un release antes de retirar |
| **Contratos** | La fuente de verdad es `argus-contracts` (nunca un servicio suelto). El móvil coordina contra este repo |
| **DB** | Un ledger `schema_version` **por servicio** (hoy: una tabla compartida + `migrate()` C++ inline). Migraciones aditivas; destructivas en 2 fases (tabla nueva + backfill + drop una versión después) |
| **Sync wire** | `TableName`, valores 0–7 y `SYNC_LIMIT=200` intactos; operaciones nuevas solo con números ≥8 |
| **Packages** | Manifiesto tipado por capacidad (`[llm] model_path, accepted_models, defaults`) sin DB; `apiVersion` SemVer; consumidores declaran rango (`llm: ^1.2`) |
| **Plugins** | `manifest.json` (vistas declarativas, permisos, `requires`) + `logic.js` + assets; firmado ed25519, verificado antes de instalar; resolutor reutiliza capacidades ya instaladas |
| **Modelos** | Intercambiables por hardware (`detect-hardware.sh` → tier CUDA > Vulkan > CPU); el package declara modelos aceptados |

---

## 6. Docker / despliegue

- **Imagen por servicio**, multi-stage sobre una base común `argus-runtime` (deps Conan
  prebuilt → cache de capas). Cada imagen compila los submodules que necesita (la de camera
  lleva ncnn; la de llm, llama.cpp; etc.). Non-root, read-only rootfs, `EXPOSE` solo gRPC.
- **Redes**: `edge` (solo gateway expuesto), `internal` (gRPC mTLS + NATS), `media` (go2rtc
  RTSP/API — el media plane aislado del plano de control).
- **Secrets de Docker**: `jwt.secret`, `jwt.refresh_secret`, `device.fingerprint_secret`,
  credenciales de cámara, claves de firma de plugins. Nada en imagen ni repo.
- **Volumen read-only compartido** para `models/` (13 GB): un solo lugar que actualiza el
  instalador por capacidad (`setup.sh camera|llm|vlm|tts|stt|memory|face`).
- **GPU**: `/dev/dri` + Mesa del host en las imágenes de camera/vlm (Vulkan RADV) — validar
  RADV dentro del contenedor antes de la Fase 2.
- **Límites `cpus`/`mem_limit` por servicio**: el consumo descontrolado se vuelve
  técnicamente impuesto por el orquestador, no convención.
- **Compose primero; Swarm solo al multi-nodo** (ej. nodo GPU + nodo cámaras). El diseño es
  orquestador-agnóstico: cambiar de compose a Swarm no toca código.
- Healthchecks gRPC (`grpc_health_v1`) en todos; restart policies; logs estructurados.
- **Instalador** (evolución de `setup.sh`): detecta hardware, descarga modelos por capacidad a
  los volúmenes, genera secrets, escribe `config/<servicio>.toml` (0600), hace `docker compose
  up`. Cada servicio publica su lista de modelos aceptados en su manifiesto package.
- **Túnel propio** (§3.7, repo `argus-tunnel`): el cliente corre como contenedor en el hogar,
  unido **solo** a la red `edge` junto al gateway; el relay corre en el **servidor dedicado de
  EE.UU.** con su propio compose (máquina exclusiva del túnel, desplegada también desde
  `argus-deploy`). Nunca se expone NATS/gRPC/RustFS.

---

## 7. Fases de migración (strangler)

> Invariante de cada fase: **la app móvil real funciona sin actualizarse**. El gateway enruta
> por prefijo de path idéntico al actual; lo no migrado sigue yendo al monolito legacy.

### Fase 0 — Preparación (en el monolito, sin mover servicios)
1. Extraer `add_library(argus_common STATIC)` con las piezas transversales (ConfigService,
   ThreadBudget, HardwareProbe, BlockingTask, schema-runner, envelope, role-access, enums,
   validation) y enlazarla desde `argus-backend`. Labs siguen compilando `.cc` crudos.
2. Resolver tablas huérfanas (drop o implementar): `voice_session`, `voice_message`,
   `context_note`, `portrait_access_request`, `portrait_access_grant`; decidir el destino de
   `event` (hoy solo lectura por sync).
3. **Harness mínimo** (prerrequisito duro, sin esto no arranca nada): doctest/catch2 en
   `src/test/` con (a) tests de contrato por ruta (request → envelope exacto, códigos 404/502),
   (b) **frames de oro de `/sync`** grabados con la app real: bootstrap de las 15 tablas,
   `camera:subscribe` → binarios fMP4, `voice:start` → PCM. Quedan en
   `argus-contracts/fixtures/`.
4. CI (GitHub Actions): build + tests + clang-format + `buf breaking`.
5. Crear `argus-contracts`: `.proto` v1 por dominio, tomando los DTO/`SyncOperation`/`TableName`
   actuales como referencia semántica.

### Fase 1 — Gateway + base
- `argus-gateway` (Drogon): mismo listener TLS 7024, misma CA/`CertService`, filtros tal cual,
  `/pairing`, `/auth/*` (login facial con FaceService + `identity.db`), `/invitation/resolve`,
  mdns, y **dueño exclusivo de `/sync`** (fan-out de NATS + proxy binario de media y voz).
- Reverse-proxy a servicios con el patrón oficial `SimpleReverseProxy`
  (`setPassThrough(true)`): no mutar headers, no transformar multipart.
- Identidad interna: metadata gRPC mTLS (`x-argus-user/role/device`) — ya no se necesita
  `trust_forwarded_for` entre gateway y servicios.
- Resto del tráfico → **legacy** (el `argus-backend` actual en contenedor, sin labs).
- `argus-deploy`: compose v1 (gateway + legacy + nats + rustfs) + secrets.

### Fase 2 — Piloto: argus-camera (aquí vive el plan YOLO26n completo)
- `camera.db` (`camera`, `camera_stream`, `zone`); endpoints `/camera*` y `/zone` con **paths
  idénticos** y distinción 404 vs 502 `CAMERA_UNREACHABLE` preservada.
- Drivers (camera-driver + tapo), go2rtc-manager/MediaRelay/StreamHub (lifecycle ya bespoke,
  fuera del registry — extracción natural).
- **YOLO26n**: diseño completo en **Appendix B** (ObjectDetectorService, CameraOperatorService,
  EventIntelligence, seam `IObjectDetector`, config e instalador). Resumen: detector ncnn
  end-to-end `(N,300,6)` sin NMS con fallback Vulkan→CPU; operador read-only que **jamás
  dispara sirena** (la prueba audible la hace el usuario personalmente); publica
  `argus.camera.v1.object_detected` a NATS → el gateway notifica con su NotificationService.
- Media: `camera:subscribe` del gateway → gRPC streaming al servicio → binarios fMP4 al
  cliente sin cambiar el protocolo.
- Labs propios del repo; `setup.sh camera` para go2rtc + artefactos del detector.
- **Validación de Vulkan en contenedor antes de esta fase.**

### Fase 3 — productivity + notification
- `argus-productivity` (`productivity.db`): calendar/project/reminder/context_note — todas
  "personal tables", mismo patrón → publican SyncOperation a NATS.
- `argus-notification` a repo/DB propia — **confirmado como servicio** (§3.7: dueño de la
  **política** de notificación; la entrega física la hará el túnel vía NATS). En Fase 3 se extrae
  con su DB y su lógica de presupuesto/digest.

### Fase 4 — Capacidades de IA (los packages)
- `argus-llm` (llama.cpp + tools + intent), `argus-vlm`, `argus-tts`, `argus-stt`: un proceso,
  un engine in-process, un conanfile y un `config/<servicio>.toml` por servicio. El
  `ai_init::llamaMutex` compartido entre LLM y VLM **desaparece** al separar procesos; los
  `cpus` de Docker imponen el budget. Tiering CUDA > Vulkan > CPU lo resuelve el instalador.
- `argus-memory` al final a propósito (el más enredado, 3.666 líneas): sus lecturas
  cross-dominio (`person`, `camera`, `zone`, `camera_stream`) se vuelven RPC o réplicas de
  lectura llenadas por eventos.
- La voz (`voice-session`) queda como orquestador en base consumiendo stt/tts/llm por gRPC;
  su extracción propia es fase futura.

### Fase 5 — Acceso remoto: túnel propio + identidad sin IP + push
- **`argus-tunnel`** (repo nuevo, se construye aquí — hoy no existe): cliente en el hogar
  (contenedor en `edge`) + relay en el **servidor dedicado de EE.UU.** (single-tenant, latencia
  genérica mundial); gRPC como protocolo del túnel; retransmite los bytes TLS del cliente
  **sin descifrar** (E2E con la CA de instancia); mantiene conexiones persistentes con los
  dispositivos ya autenticados. El relay se despliega con su propio compose desde `argus-deploy`
  y lleva healthcheck + reinicio automático (si cae, el acceso remoto cae pero la LAN no).
- **Identidad de dispositivo sin IP**: CertService emite certificados de cliente por dispositivo
  + `device_secret`; DeviceFilter pasa a `HMAC(User-Agent | credencial)` — la IP sale del hash
  (decisión del usuario). Cambio de app coordinado vía `argus-contracts`; re-emisión única de
  sesiones existentes (re-login controlado).
- **Enforcement LAN-only**: el gateway rechaza `/pairing` y `/auth/register` cuando el peer no
  está en `network.lan_cidrs` o `remote.enabled=true` — por el túnel solo pasan usuarios ya
  registrados.
- **SANs**: `remote.hostname` en los certificados; la app configura servidor manual además de
  mDNS.
- Endurecimiento: rate limiting + lockout por credencial en `/auth/refresh-token` (única
  pre-auth remota), antes de tocar SQLite.
- **Push por el túnel**: `argus-notification` publica intenciones a NATS → `argus-tunnel` las
  entrega por las conexiones persistentes de los dispositivos conectados (evento de cámara,
  presencia, alarmas de zona llegan fuera de casa).
- **Prueba de aceptación**: la app fuera de la LAN hace login facial, bootstrap de `/sync`, ve
  video y recibe push por el túnel; al volver a la LAN la sesión **no se invalida** (identidad
  sin IP).

### Fase 6 — Plugins frontend (blueprint; implementación posterior)
- Base app (React Native + Tauri): shell (configuraciones, certs/pairing, runtime de plugins,
  catálogo). Plugin = manifest + logic.js (QuickJS sandbox) + assets, firmado ed25519.
- El resolutor verifica capacidades (backend vivo por gRPC + modelo instalado), instala lo
  faltante y **reutiliza lo ya instalado** (si otro plugin ya trajo LLM, no se reinstala).
- Comunicación dispositivo-dispositivo por `/sync` con operaciones nuevas (números ≥8).
- Los permisos declarados del plugin los aplica el gateway como metadata gRPC.

---

## 8. Verificación

1. **Fase 0**: build 0 errores/0 warnings; tests verdes; `buf breaking` limpio; frames de oro
   de `/sync` grabados y versionados en `argus-contracts/fixtures/`.
2. **Por cada extracción**: suite de contratos del servicio movido pasa contra el gateway
   (envelope exacto, multipart intacto, 404 vs 502 distinguidos, **matriz de filtros de
   Appendix A idéntica ruta por ruta**, incluidas las 6 pre-auth); replay de frames `/sync`
   byte-idéntico en las operaciones migradas.
3. **App móvil real** conectada en cada fase: login facial, bootstrap de las 15 tablas, video
   de cámara y voz funcionan sin actualizar la app — LA prueba de retrocompatibilidad.
4. **Compose**: `docker compose up` levanta el stack; healthcheck gRPC por servicio; matar el
   container de un servicio de IA no tumba gateway ni cámara (aislamiento verificado).
5. **Datos**: conteos/checksums de filas migradas vs legacy antes de cortar cada dominio;
   rollback documentado por fase.
6. **Seguridad**: solo el gateway escucha en `edge`; el resto en `internal` (gRPC mTLS);
   secrets nunca en imagen/repo; `go2rtc.yaml` sigue chmod 600 en su volumen.

## 9. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| `/sync` no descompone limpio (sync + media + voz en un socket con permisos por mensaje) | El gateway lo posee **completo** y hace proxy; nunca se divide por dentro mientras existan clientes v1 |
| Device fingerprint a través de proxies | Resuelto por diseño (el gateway es el único que ve al cliente); cualquier proxy futuro exige `User-Agent` byte-exacto |
| gRPC en Conan: under-linking con `CMakeConfigDeps` (#19790) | Enlazar explícitamente `gRPC::grpc++ gRPC::grpc gRPC::gpr protobuf::libprotobuf`; protoc pinneado EXACT |
| Sin tests hoy | Harness de Fase 0 = prerrequisito duro de todas las fases |
| Memory lee tablas ajenas por SQL directo (`memory-graph-query.hxx:171`) | RPC o réplicas de lectura por eventos; se extrae al final a propósito |
| CORS `*` + OPTIONS short-circuit hoy | El gateway lo replica exacto (o headers duplicados) |
| Login facial in-proc (HNSW + ncnn) | Va con el gateway en Fase 1, no después |
| Imágenes grandes (submodules + modelos) | Base `argus-runtime` en cache de capas; modelos en volumen read-only |
| Vulkan en contenedor (RADV) | `/dev/dri` + Mesa del host; validar dentro del contenedor antes de Fase 2 |
| SQLite: una DB por servicio es la escalabilidad correcta | Cada servicio con su WAL y su lock; FKs cross-dominio → ids validados en el servicio dueño |
| **Identidad por IP (HMAC UA\|IP)**: la IP cambia por red y con el túnel todos los remotos comparten la del relay → 401 "Device mismatch" y colisiones | Credencial por dispositivo sin IP (cert mTLS + `device_secret`); hash = HMAC(UA\|credencial) — Fase 5, con re-login controlado una vez |
| El túnel propio descifra (rompe el E2E y el pinning de la CA) | El túnel solo **retransmite bytes**; dos TLS distintos (cliente↔gateway E2E, y túnel↔gateway interno si usa gRPC) |
| El relay es punto único de falla del acceso remoto (servidor dedicado en EE.UU.) | Healthcheck + auto-restart; si cae, solo el acceso remoto se pierde — la LAN y todo el stack local siguen funcionando; el push local no depende del relay |
| Superficie pre-auth remota: `/auth/refresh-token` queda expuesta (y `/pairing`/`/auth/register` si no se refuerzan) | `/pairing` y `/auth/register` **LAN-only** por CIDRs (decisión del usuario); rate limiting + lockout en refresh-token antes de tocar SQLite; `/auth/has-admin` eliminada |
| SANs actuales sin hostname público del relay → TLS mismatch desde internet | `remote.hostname` en las SANs vía CertService; la app acepta servidor manual para remoto |
| `JwtFilter` = 2 queries SQLite por request; un flood desde internet golpea `identity.db` | Rate limit en el edge **antes** del filtro; lockout; WAL + busy_timeout en identity.db |

## 10. Decisiones abiertas (para el usuario)

1. **Tablas huérfanas**: ¿drop (`voice_session`, `voice_message`, `context_note`,
   `portrait_access_request/grant`) o implementarlas?
2. **Registry Conan**: ¿`local_recipes_index` local para arrancar y Artifactory CE después, o
   Artifactory CE desde el día 1?
3. **Plugin runtime móvil**: ¿QuickJS embebido (recomendado) vs. solo manifiesto declarativo
   sin lógica custom?
4. **Yolo26n dentro de `argus-camera`**: confirmar que el plan YOLO26n (ObjectDetectorService +
   operator + intelligence) se implementa como capacidad `camera.objects` en Fase 2, tal como
   estaba planeado (detalle completo en Appendix B).
5. ~~Dónde corre el relay del túnel~~ **DECIDIDO**: servidor dedicado en la nube, EE.UU.,
   single-tenant (solo el túnel), latencia genérica mundial. Queda por definir el detalle del
   protocolo del túnel (gRPC confirmado como base) y su mecanismo de fallback si el relay cae.

---

## Appendix A — Inventario completo de rutas (recontado en código)

49 rutas HTTP vía `ADD_METHOD_TO` (grep verificado) + 1 WS (`/sync`, `sync-socket.hxx:18`).
Abreviaturas: **D**=DeviceFilter · **V**=ValidJsonFilter · **J**=JwtFilter · **R**=RoleFilter ·
**DVR** completa = D+V+J+R. `/auth/has-admin` **se elimina** del código (decisión del usuario),
incluido su handler y `AuthService::hasAdmin` — el bootstrap de `/auth/register` usa su propio
chequeo transaccional de owner.

**auth (9)**
| Método | Path | Filtros | Handler |
|---|---|---|---|
| POST | `/auth/login` | D | login (multipart con imagen → facial) |
| POST | `/auth/register` | D | register (multipart; bootstrap owner/invitación) — **LAN-only en Fase 5** |
| GET | `/auth/status` | D+J | status |
| POST | `/auth/device-login` | D | createDeviceLogin (QR, TTL 120 s) |
| POST | `/auth/device-login/{1}/approve` | D+J | approveDeviceLogin |
| GET | `/auth/device-login/{1}` | D | pollDeviceLogin |
| PATCH | `/auth/refresh-token` | D+V | refreshToken (single-use, pre-auth: **sin JwtFilter**) |
| PATCH | `/auth/logout` | D+J | logout |
| PATCH | `/auth/me` | D+V+J | updateMe |

**pairing (1)** — POST `/pairing`, solo V (bootstrap PKI sin auth; 409 si ya paired) — **LAN-only en Fase 5**
**invitation (4)** — POST `/invitation/resolve` solo V (onboarding sin auth) · GET `/invitation`
D+J+R · POST `/invitation` DVR · DELETE `/invitation/{1}` D+J+R

**user (3) + portrait-preview (2)**
| Método | Path | Filtros | Handler |
|---|---|---|---|
| GET | `/user` | D+J+R | list — **la única lista HTTP de la app** |
| PATCH | `/user/{1}` | DVR | update |
| DELETE | `/user/{1}` | D+J+R | deactivate |
| GET | `/portrait-preview/{1}` | D+J+R | create (emite token de preview) |
| GET | `/portrait-preview/{1}/content` | D+J+R | consume |

**camera (3)** — POST `/camera` DVR · PATCH `/camera/{1}` DVR · DELETE `/camera/{1}` DVR
(lectura de la lista **no existe** por HTTP: va por `/sync`)

**camera-control (7)**
| Método | Path | Filtros |
|---|---|---|
| GET | `/camera/{1}/status` | D+J+R |
| GET | `/camera/{1}/presets` | D+J+R |
| PATCH | `/camera/{1}/ptz` | DVR |
| PATCH | `/camera/{1}/preset` | DVR |
| PATCH | `/camera/{1}/settings` | DVR |
| GET | `/camera/{1}/capabilities` | D+J+R |
| POST | `/camera/{1}/talk` | DVR (toca TtsService) |

**zone (3)** — POST `/zone` DVR (valida cameraId → 404) · PATCH `/zone/{1}` DVR · DELETE `/zone/{1}` DVR

**calendar-event (3)** — POST/PATCH/DELETE, todos DVR
**calendar-event-share (3)** — POST/PATCH/DELETE, todos DVR
**project (3)** — POST/PATCH/DELETE, todos DVR
**project-member (3)** — POST/PATCH/DELETE, todos DVR
**project-task (3)** — POST/PATCH/DELETE, todos DVR

**notification (2)** — PATCH `/notification/read` DVR · POST `/notification-token` DVR

**WS (1)** — `/sync` con D+J (sin RoleFilter: los permisos por tabla se aplican **dentro**,
por rooms y `role-access`).

Patrón global: toda ruta de escritura lleva la cadena completa D+V+J+R; las de solo-lectura se
ahorran V; solo 6 rutas (las pre-auth listadas en §2.3) omiten J. El gateway debe preservar esta
matriz **exacta** — `role-access.hxx` mapea cada path a su tabla y el sync comparte ese mapa.
Con el túnel (Fase 5): `/pairing` y `/auth/register` solo responden a peers de LAN; el resto de
la matriz funciona igual dentro y fuera del hogar porque la identidad deja de depender de la IP.

---

## Appendix B — Piloto cámara: plan YOLO26n completo (se implementa en Fase 2 dentro de argus-camera)

Estado real (verificado hoy): **diseño completo, cero código**. `src/shared/services/
camera-operator/` existe **vacío**; no hay `yolo26n.param/bin` en `models/`, no hay
`setup_object_model` en `scripts/setup.sh`, no hay secciones `[objects]`/`[operator]` en
`config.toml`. Se implementa directamente sobre la estructura de microservicios.

### B.1 ObjectDetectorService (ncnn)

- YOLO26n **end-to-end**: entrada letterbox (padding gris **114**) a la resolución nativa del
  modelo; salida directa `(N,300,6)` = `[cx, cy, w, h, score, cls]` — **sin NMS ni post-proceso
  de anclas** (`nms=False` en el export).
- Los blob names de entrada/salida se **leen del `.param`** en runtime (no hardcodeados: varían
  según el export).
- **Fallback Vulkan→CPU por instancia** si la GPU falla en runtime (el detector degrada, el
  servicio no muere).
- Semáforo de inicialización **liberado solo cuando el init es exitoso** — patrón que corrige el
  deadlock latente de FaceService (retiene el semáforo si la carga del modelo falla y bloquea
  todos los logins siguientes).
- Config (sección nueva `[objects]` en `config/camera.toml`): `model` (ruta), `classes`,
  `input_size`, `conf`, `enabled`, `max_fps_inference`.
- Instalador: `setup.sh camera` descarga `yolo26n.param` + `yolo26n.bin` con sha256 y
  `.part`+rename atómico (misma disciplina que los demás modelos — `setup_object_model`).

### B.2 CameraOperatorService

- Loop por cámara como **coroutine en el event loop de Drogon**; el trabajo pesado (preproceso +
  inferencia) va a `BlockingTask` — nunca bloquea el loop.
- `ICameraDriver::events`: los drivers reportan eventos; para el cliente Tapo el walker de la
  lista de detección es **tolerante a forma** (el SDK puede cambiar el shape del payload
  `searchDetectionList`) con fallback `getLastAlarmInfo` cuando el push falla.
- **El operador es read-only respecto al hardware**: jamás llama `setAlarm`, sirena ni ningún
  comando audible. La sirena se prueba **personalmente por el usuario** — ninguna ruta de código
  (operador, EventIntelligence ni notificaciones) debe dispararla. Única acción: publicar a
  NATS y notificar.
- Overlay/watermark opcional sobre el frame analizado y dedupe (solo keyframes o salto
  configurable por `max_fps_inference`).

### B.3 EventIntelligence (9 reglas, evaluadas en orden)

| # | Regla | Severidad/acción |
|---|---|---|
| 1 | `exclude_zone` | descarta el evento |
| 2 | `known_person` | notificación personalizada ("David llegó a casa") |
| 3 | `person_in_alert_zone` | **Critical** |
| 4 | `person_in_monitor_zone` | Warning |
| 5 | `person_night` | Warning |
| 6 | `person_day` | Info |
| 7 | `vehicle_arrival` | Info |
| 8 | `vehicle_night` / presencia escalando | Warning + escalado |
| 9 | `ignored_class` | descarta |

- **Identidad primero**: si el crop pasa por FaceService (persona conocida), `known_person`
  domina la severidad de las reglas 3–8.
- Agregación: ventana de agregación por cámara; **cooldown por cámara+clase**;
  **presupuesto de notificación 6/hora** con digest acumulativo cuando se excede;
  **horas de silencio** configurables.
- Config (sección `[operator]` en `config/camera.toml`): zonas por cámara (alert/monitor/
  exclude), horarios, presupuesto, cooldown.

### B.4 Seam, licencia y salida

- **`IObjectDetector`** como única interfaz del servicio de detección: YOLO26n es **AGPL-3.0**
  — para distribución comercial se reemplaza el artefacto del modelo (p. ej. RF-DETR-Nano,
  licencia permisiva) **sin tocar el servicio**; NOTICE/atribución de terceros vive en
  `argus-camera`.
- Salida: evento `argus.camera.v1.object_detected` a NATS (JetStream) → el gateway consume,
  aplica presupuesto/horas-silencio y notifica con su NotificationService.
- Labs propios del repo: `object-bench` (latencia/fps del detector por tier de hardware) y
  `camera-probe`.