# PLAN — Optimización adaptativa + Memoria de personas

> **Fuente única de verdad** para las siguientes dos iteraciones del backend Argus. Sustituye a
> `CAMERA_INTEGRATION_PLAN.md` (borrado: su Fase 1 ya está implementada en
> `src/shared/services/tapo/`, y el resto se reordena aquí).
>
> Dos objetivos que se refuerzan entre sí:
>
> 1. **Rendir al máximo en cualquier hardware sin sacrificar calidad** — adaptación en build, boot y
>    runtime, con una política explícita de qué se sacrifica y qué no.
> 2. **Memoria de personas** — rostros con contexto acumulado, tags, eventos por persona y ejecución
>    de *tools* por el LLM sin las pasadas de inferencia ni el parseo frágil del patrón convencional.
>
> Principio transversal: **menos pasos = más tiempo real.** No se optimiza haciendo cada paso más
> rápido, se optimiza borrando pasos.

---

## Índice

0. [Punto de partida](#0-punto-de-partida)
1. [Principios de diseño](#1-principios-de-diseño)
2. [Eje A — Adaptación a cualquier hardware](#2-eje-a--adaptación-a-cualquier-hardware)
3. [Eje B — Eliminación de pasos](#3-eje-b--eliminación-de-pasos)
4. [Eje C — Memoria de personas](#4-eje-c--memoria-de-personas)
5. [Eje D — Higiene del repositorio](#5-eje-d--higiene-del-repositorio)
6. [Eje E — LLM y VLM: optimización interna](#6-eje-e--llm-y-vlm-optimización-interna)
7. [Componentes a implementar](#7-componentes-a-implementar)
8. [Configuración](#8-configuración)
9. [Fases](#9-fases)
10. [Riesgos](#10-riesgos)
11. [Puntos abiertos](#11-puntos-abiertos)
12. [Fuentes verificadas](#12-fuentes-verificadas)

---

## 0. Punto de partida

### 0.1 Lo que ya está implementado

`src/shared/services/tapo/` — **2991 líneas**, la Fase 1 del plan anterior completa:

| Archivo | Contenido |
|---|---|
| `tapo-transport.hxx` | Interfaz `ITapoTransport` |
| `secure-passthrough-transport.*` | Transporte primario (`stok` + `securePassthrough`, `lsk`/`ivb`, `Seq`, `Tapo_tag`) |
| `legacy-stok-transport.*` | Fallback de firmware antiguo |
| `tapo-client.*`, `tapo-api.*`, `tapo-http.*`, `tapo-crypto.*` | Cliente serializado + métodos tipados + cripto OpenSSL |
| `tapo-talk-client.*`, `tapo-ts-muxer.*`, `tapo-audio.*` | Canal `talk` del 8800 + muxer MPEG-TS PCMA |
| `labs/tapo-probe/` | Binario `argus-tapo-probe` (450 líneas) validado contra la C225 real |

También existe `src/shared/wrapper/cancellation/` (`CancellationToken`).

### 0.2 Hallazgos que corrigen el plan anterior

| # | Hallazgo | Consecuencia |
|---|---|---|
| 1 | `AGENTS.md` **regla 13b**: *"NEVER hardcode thread counts. All AI services size their thread pools from `thread-budget.hxx`"* | El bloque `[threads]` con valores fijos del plan anterior **violaba una regla del proyecto**. El objetivo 1 de este plan no es una feature nueva: es **extender una convención existente** a los aceleradores |
| 2 | `LlmService` es una clase **estática** con un único `context_`, un `std::mutex` global y `ChatRequest::resetContext{true}` por defecto | El "KV cache por sesión" exige refactor, no un ajuste. Pero la API de `llama_memory_seq_*` permite algo mejor que lo planificado (§4.5) |
| 3 | `FaceService::identify(imageBytes)` **decodifica** la imagen (`stbi_info` + OpenCV) | Pasarle un crop obliga a **codificarlo primero**: dos pasos de desperdicio puro en el camino caliente de identidad (§3.2) |
| 4 | `VisionService` ya cachea la salida del encoder **por hash de imagen** | Prior art reutilizable: hashear el crop del track hace que re-describirlo sea gratis |
| 5 | `AGENTS.md` regla 17: Release usa `-march=native` + `-flto=auto`, con build en la máquina destino | La optimización por hardware **a nivel de compilación ya existe**. Lo que falta es la de *configuración* (opciones de Conan) y *runtime* |
| 6 | El esquema ya tiene `person.observation`, `face_embedding.angle_label`/`quality`, `person_event.confidence`, `context_note.tags`/`valid_from`/`valid_until` | La memoria de personas necesita **1 tabla nueva**, no un rediseño (§4.1) |

### 0.3 Lo que se conserva del plan anterior

Las decisiones de arquitectura de media siguen vigentes y no se repiten aquí en detalle:
1 sesión RTSP por cámara vía go2rtc en passthrough; decodificación H.264 acelerada por hardware;
WebRTC/WHEP al móvil con Argus como proxy de señalización SDP; MJPEG descartado; `IDetectBackend`
con ncnn/Vulkan primario y ORT-CPU como fallback y oráculo de tests; ByteTrack obligatorio; crop a
resolución completa en dos etapas para cara y VLM.

---

## 1. Principios de diseño

### 1.1 Pisos de calidad, palancas de throughput

En hardware débil no se puede tener todo. La política es explícita y se aplica en todo el plan:
**se sacrifica latencia y frecuencia de análisis antes que precisión.** Dos fps que reconocen a la
persona valen más que ocho que la pierden.

| | Qué es | Ejemplos |
|---|---|---|
| **Pisos** — nunca escalan hacia abajo | Todo lo que produce **daño acumulativo** si se degrada | Resolución del crop facial · modelo de embedding · umbrales de `CropQuality` · umbrales de confianza del detector · corrección de los eventos |
| **Palancas** — escalan libremente | Todo lo que solo produce **pérdida transitoria** | `analysis_fps` · `input_size` (dentro de límites) · VLM on/off y resolución · tamaño del LLM · fps del stream · concurrencia |

La razón del piso facial: **un embedding malo contamina el índice HNSW de forma permanente.** No es
un frame perdido, es una identidad equivocada que persiste y se propaga a todos los eventos futuros
de esa persona. Un frame perdido se recupera en 125 ms; una identidad equivocada, nunca.

### 1.2 Menos pasos = más tiempo real

Todo el eje B es esto. Cada optimización de esa sección **borra trabajo**, no lo acelera:
una copia de buffer que no se hace, un encode/decode que no ocurre, una pasada de inferencia que no
se lanza, un `fsync` que se agrupa.

### 1.3 La IA es el camino de excepción, no el pipeline

El objetivo funcional no es "una cámara con IA encima". Es que **el >95% de los eventos se resuelvan
sin invocar ningún modelo**, con tags y reglas, y que los modelos entren solo cuando aportan algo que
las reglas no pueden dar. Ver la escalera de decisión (§4.7).

---

## 2. Eje A — Adaptación a cualquier hardware

### 2.1 `HardwareProfile`

`src/shared/wrapper/hardware-profile/hardware-profile.hxx/.cc` — **extiende** `ThreadBudget`, no lo
reemplaza. `ThreadBudget` sigue siendo la fuente de verdad para hilos (regla 13b); `HardwareProfile`
añade lo que hoy no sabe: aceleradores, ISA y memoria.

```cpp
enum class VideoAccel { None, Vaapi, Nvdec, Qsv, VideoToolbox };
enum class CapabilityTier { Minimal, Low, Balanced, High };

struct HardwareProfile
{
  int      physicalCores{1};
  int      logicalThreads{1};
  bool     avx2{false};
  bool     avx512{false};
  bool     fma{false};
  bool     neon{false};
  int64_t  ramTotalMb{0};
  int64_t  ramAvailableMb{0};

  bool     vulkan{false};
  bool     vulkanDiscrete{false};
  std::string vulkanDevice;
  int64_t  vulkanVramMb{0};

  VideoAccel  videoAccel{VideoAccel::None};
  std::string videoDevice;

  CapabilityTier tier{CapabilityTier::Minimal};
};

namespace HardwareProbe
{
  const HardwareProfile& get();   // sondeo una sola vez, thread-safe, cacheado
  Json::Value toJson();           // para /metrics y para hw-probe
}
```

**Cómo se sondea cada campo, sin añadir dependencias:**

| Campo | Mecanismo | Coste |
|---|---|---|
| `logicalThreads` | `std::thread::hardware_concurrency()` (ya lo usa `ThreadBudget`) | 0 |
| `physicalCores` | `/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`, con fallback a `logicalThreads` | ~1 ms |
| `avx2` / `avx512` / `fma` | `__builtin_cpu_supports("avx2")` — GCC intrínseco, cero coste en runtime | 0 |
| `ramTotalMb` | `sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE)` | 0 |
| `vulkan*` | **`ncnn::get_gpu_count()` + `ncnn::get_gpu_info(i)`** → nombre, `type` (0=discreta/1=integrada), presupuesto de heap. **ncnn ya está enlazado con `NCNN_VULKAN=ON`** | ~50 ms (una vez) |
| `videoAccel` | `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VAAPI / CUDA / QSV)` en orden, primero que abra gana | ~100 ms (una vez) |

El sondeo corre **una vez** dentro de la inicialización paralela de `ServiceRegistry`, y el resultado
se expone en `/metrics` y en el binario `labs/hw-probe`.

### 2.2 La adaptación ocurre en tres momentos

Este es el hueco del plan anterior: solo contemplaba el tercero.

| Momento | Quién decide | Qué decide |
|---|---|---|
| **Build** — `scripts/setup.sh` | Detección de hardware → **opciones de Conan** | `llama-cpp/*:with_vulkan` · `llama-cpp/*:with_cuda` · `opencv/*:with_ffmpeg` · flags de ncnn. Verificado: son opciones reales del recipe (§12) |
| **Boot** — `HardwareProfile` → `tier` | Selección de backends y modelos | Backend del detector · qué modelos se cargan · `analysis_fps` inicial · VLM on/off · hilos vía `ThreadBudget` |
| **Runtime** — métricas del pipeline | Degradación reactiva | Si `argus_detector_ms` p95 supera 2× el objetivo, baja `analysis_fps` **antes** de acumular cola. Si vuelve a la normalidad durante N s, sube |

El primer momento es el que más rinde y es prácticamente gratis, **porque ya compilas en la máquina
destino** (regla 17). Un `llama-cpp` con Vulkan en una máquina con dGPU no es una opción de runtime:
es una decisión de build que hoy no se está tomando.

`setup.sh` gana una función:

```bash
detect_hardware() {
  # Escribe scripts/.hw-profile con: TIER, HAS_VULKAN, HAS_CUDA, VIDEO_ACCEL, CORES, RAM_MB
  # Fuentes: nproc · /proc/meminfo · vulkaninfo|ncnn · /dev/dri/renderD* · nvidia-smi
  # Deriva las opciones de Conan y las escribe en el perfil o en la línea de conan install.
}
```

Con `ARGUS_TIER=<tier>` como override manual para pruebas y para máquinas donde la detección
acierte mal.

### 2.3 Tiers

| | **Minimal** (≤2 cores, sin GPU) | **Low** (4 cores) | **Balanced** (≥8 cores + iGPU) | **High** (dGPU ≥4 GB) |
|---|---|---|---|---|
| Backend detector | `ort_cpu` INT8 | `ort_cpu` | `ncnn_vulkan` | `ncnn_vulkan` |
| `input_size` | 512 | 512 | 512 | 640 |
| `analysis_fps` | 2 | 4 | 8 | 12 |
| Decodificación | software + drop de frames | software | VAAPI / QSV | NVDEC |
| **Cara** | **piso** | **piso** | **piso** + Vulkan | **piso** + Vulkan |
| VLM | **off** (solo reglas) | on demand | 512 px | 512 px |
| LLM | LFM2.5-350M | LFM2.5-1.2B | LFM2.5-1.2B | 1.2B + offload GPU |
| STT | `nemo_ctc` | `nemo_transducer` | `nemo_transducer` | `nemo_transducer` |
| Conversación | deshabilitada | opt-in | on | on |

Dos cosas que leer con atención en esa tabla:

- **La fila de cara no escala.** Es el piso de §1.1. En Minimal se pierde fluidez, no identidad.
- **`input_size` se mantiene en 512 incluso en Minimal.** Lo que baja es `analysis_fps`. Bajar la
  resolución de entrada a 320 haría al detector perder personas pequeñas o lejanas de forma
  sistemática — eso sí es perder calidad. Bajar los fps solo retrasa la detección.

En Minimal el VLM se apaga en lugar de reducirse: una descripción de escena mala es peor que ninguna,
porque entra en el contexto del LLM como si fuera verdad y contamina las decisiones.

### 2.4 Manifest de modelos por tier

`models/manifest.toml` — `setup.sh` descarga **solo lo que el tier necesita**, en lugar de todo. En
una máquina Minimal eso ahorra el modelo grande de LLM y los pesos del VLM.

```toml
[detector.yolo26n]
tiers  = ["minimal", "low", "balanced", "high"]
format = ["onnx", "ncnn"]          # ncnn solo si HAS_VULKAN
[detector.yolo26n.int8]
tiers  = ["minimal"]

[llm."LFM2.5-350M"]
tiers = ["minimal"]
[llm."LFM2.5-1.2B"]
tiers = ["low", "balanced", "high"]

[vision.smolvlm]
tiers = ["low", "balanced", "high"]
```

---

## 3. Eje B — Eliminación de pasos

Resumen de lo que borra cada camino:

| Camino | Pasos antes | Pasos después | Qué se borra |
|---|---|---|---|
| Vídeo → tensor | 8 pasadas de buffer | **3** | 2 resizes, 1 conversión de color, 1 buffer de gate |
| Crop → identidad | 5 | **3** | 1 encode + 1 decode |
| Identidad por track | 1 por frame | **1 por track** | ~7 embeddings de cada 8 frames |
| Decisión | reglas → LLM | **escalera de 4 puertas** | >95% de las inferencias |
| Turno de LLM | 2-3 pasadas | **1** | el re-prefill y la pasada de resultado de tool |
| Escritura de eventos | 1 tx por evento | **1 tx por lote** | N-1 `fsync` por ráfaga |

### 3.1 Ruta de vídeo: de 8 pasadas a 3

**Antes** (plan anterior): `decode → cv::Mat BGR full-res → resize 512 → buffer de gate 160 →
absdiff → letterbox → normalizar → blob float NCHW`.

**Después:**

```
1. El decodificador entrega NV12 (formato nativo de VAAPI/NVDEC — no se convierte a BGR)
2. Motion gate sobre el PLANO Y con stride:
     el plano Y de NV12 ya es luminancia de 8 bits contigua.
     Se recorre con paso (stride) para muestrear ~160 px de ancho.
     → 0 copias, 0 resize, 0 asignaciones.  ~1-3 ms → ~0.2 ms
3. Si hay movimiento: UN solo cv::warpAffine que hace
     downscale + letterbox + conversión de color, escribiendo
     DIRECTAMENTE en el buffer de entrada de la red.
```

El BGR full-res **solo se materializa cuando hay un crop que extraer** (cara o VLM), que ocurre una
vez por track, no una vez por frame. En el 95% de los frames (los que el gate descarta) no se toca
nada más allá del plano Y.

### 3.2 Ruta de identidad: borrar un encode y un decode

`FaceService::identify(imageBytes)` decodifica internamente. Para pasarle un crop habría que
**codificarlo a JPEG/PNG primero**, con lo que el camino sería:

```
cv::Mat crop → cv::imencode(JPEG) → bytes → stbi/OpenCV decode → cv::Mat → RetinaFace → embedding
                └──────────────── dos pasos de desperdicio puro ────────────────┘
```

**Cambio**: añadir a `FaceService` una entrada que acepte píxeles crudos, conservando la existente
para el login por multipart (que sí recibe bytes de verdad):

```cpp
FaceResult              extractMat(const cv::Mat& bgr);
std::optional<int64_t>  identifyMat(const cv::Mat& bgr);
drogon::Task<std::optional<int64_t>> identifyMatAsync(const cv::Mat& bgr);
```

Mismo tratamiento para `VisionService`: entrada por `cv::Mat` para que el VLM reciba el crop sin
pasar por un códec. La caché por hash de imagen que ya existe se alimenta hasheando los píxeles del
crop.

### 3.3 Embedding: uno por track, con trinquete de calidad

En vez de re-embeber en cada frame:

```
track.personId == nullopt  → intentar identificar (con backoff, max_face_attempts)
track.personId != nullopt  → NO volver a embeber… salvo que:
      CropQuality(nuevo) > CropQuality(guardado) + margen
        → re-embeber y REEMPLAZAR el embedding almacenado
```

El trinquete hace que la identidad **mejore sola** a lo largo de la presencia de la persona, a coste
cercano a cero: el crop ya estaba calculado y la comparación es un float. Y es la base del
enrolamiento multi-ángulo (§4.4): los tres mejores crops por ángulo salen de este mismo mecanismo.

### 3.4 Ruta de datos: agrupar escrituras

- **Ring buffer de eventos** con flush en **una sola transacción** cada `flush_interval_ms` (o al
  llenarse). Una ráfaga de 10 detecciones pasa de 10 `fsync` a 1.
- **`ai_event` sale por `SocketService::emitModule` directo**, sin pasar por la maquinaria de diffs y
  auditoría del motor de sync. La copia durable va a `event` para la sincronización posterior.
- La auditoría (`audit_log` / `user_audit_log`) se reserva para **acciones de usuario**, no para
  detecciones automáticas. Auditar cada detección infla la BD y no aporta trazabilidad útil.

### 3.5 Degradación reactiva en vez de cola creciente

Un buffer que crece convierte un pico de carga en latencia permanente. La política:

```
cola decoder→análisis = 1 frame con REEMPLAZO   (siempre el más reciente)
si p95(detector_ms) > 2 × objetivo del tier durante 5 s:
      analysis_fps -= 1    (piso: 1)
si p95(detector_ms) < objetivo durante 30 s:
      analysis_fps += 1    (techo: el del tier)
```

El sistema **pierde frames, no latencia**. Y lo cuenta:
`argus_frames_dropped_total{camera,stage}`.

---

## 4. Eje C — Memoria de personas

### 4.1 El esquema ya está diseñado para esto

| Tabla existente | Lo que ya tiene | Para qué se usa aquí |
|---|---|---|
| `person` | `name`, `alias`, **`observation`** (TEXT libre), `first_seen_at`, `last_seen_at` | Identidad + **perfil compactado que mantiene el LLM** |
| `face_embedding` | `embedding` BLOB, **`angle_label`**, **`quality`** | **Enrolamiento multi-ángulo** con puntuación |
| `event` | `event_type`, `severity`, `source`, `summary`, **`details` JSON**, `occurred_at` | Qué pasó |
| `person_event` | `person_id` + `event_id` + **`confidence`** | **La línea de tiempo por rostro** |
| `context_note` | `title`, `content`, **`tags`**, `valid_from`, `valid_until`, `is_active` | Hechos globales con vigencia temporal |
| `zone` | `points` normalizados, `zone_type` (monitor/alert/exclude) | ROI y reglas espaciales |

**Migración necesaria: una tabla.**

```sql
CREATE TABLE IF NOT EXISTS person_tag (
    person_id  INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    tag        TEXT    NOT NULL,
    source     TEXT    NOT NULL  DEFAULT 'llm'
                                 CHECK (source IN ('llm', 'user', 'rule')),
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (person_id, tag)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_person_tag_tag ON person_tag (tag);
```

Tabla propia en vez de una columna `tags` en `person` porque el lookup por tag tiene que ser O(1) e
indexable: es la **puerta 0** de la escalera de decisión (§4.7) y corre en cada detección.
`source` permite distinguir lo que puso el usuario de lo que infirió el modelo, y por tanto permitir
que el usuario tenga la última palabra.

Sigue la regla 1 de `AGENTS.md`: el `CHECK` de `source` necesita su `enum class TagSource` en
`src/shared/enums.hxx`.

### 4.2 Vocabulario de tags

Cerrado y versionado, no libre. Un vocabulario abierto hace que el modelo invente sinónimos
(`familia` / `familiar` / `pariente`) y rompe las reglas que dependen de ellos.

| Categoría | Tags | Efecto en reglas |
|---|---|---|
| Relación | `residente` `familia` `amigo` `trabajador` | `residente`/`familia` → sin alerta |
| Función | `repartidor` `servicio` `mantenimiento` `visita` | Alerta informativa, ventana horaria esperada |
| Seguridad | `autorizado` `vetado` `sospechoso` | `vetado` → **crítico inmediato**, sin pasar por más puertas |
| Atributo | `menor` `mayor` `con-mascota` `con-vehiculo` | Modifica el prompt y la severidad |

El vocabulario vive en un solo sitio (`src/shared/services/memory/tag-vocabulary.hxx`) y de ahí
salen **tres** consumidores: el `enum`, la gramática GBNF del LLM (§4.3) y el motor de reglas.
Añadir un tag es editar una línea.

### 4.3 Tools: por qué no el patrón convencional

**Convencional**: el modelo emite JSON → se parsea → a veces falla → se reintenta → se ejecuta → se
devuelve el resultado al modelo → **el modelo genera otra vez**. Dos o tres pasadas de inferencia
por tool call, y un parser que puede fallar en producción.

Cuatro mecanismos, **los cuatro nativos en el pin `llama-cpp/b6565`** (verificado en el header, §12):

#### (a) Gramática perezosa + DSL compacto

`llama_sampler_init_grammar_lazy_patterns(vocab, grammar, root, trigger_patterns, trigger_tokens)`
deja al modelo generar **habla libre y natural**, y **solo activa la gramática cuando aparece el
disparador**. Dentro del tool call, emitir algo inválido es estructuralmente imposible: el sampler no
deja.

> ✅ **El disparador no hay que inventarlo: el modelo ya lo tiene.** Al auditar el GGUF aparecieron
> tokens **nativos de tool-calling** en el vocabulario de LFM2.5:
> `<|tool_call_start|>`, `<|tool_call_end|>`, `<|tool_list_start|>`, `<|tool_list_end|>`,
> `<|tool_response_start|>`, `<|tool_response_end|>`.
>
> Usar `<|tool_call_start|>` como `trigger_token` es estrictamente mejor que un `»` inventado:
> es **un solo token dedicado** (no hay riesgo de que se parta en varios), el modelo fue **entrenado**
> para emitirlo cuando toca llamar a una herramienta, y no puede aparecer por accidente en texto
> natural. Esto **cierra el punto abierto nº1** de la versión anterior de este plan.

```
JSON convencional:
  {"name":"save_face","arguments":{"track_id":17,"name":"María","tags":["familia"]}}
DSL de Argus:
  <|tool_call_start|>save T17 "María" #familia<|tool_call_end|>
```

Estimación por conteo de caracteres (a medir en Fase G): **~38 → ~12 tokens**, unas 3× menos en la
ruta de tool. Y **cero reintentos, siempre** — no "casi siempre".

#### (b) La gramática se genera por request con el estado vivo

Esta es la propiedad más valiosa del diseño:

```gbnf
root     ::= call+
call     ::= (save | tag | note | log | recall | forget) "<|tool_call_end|>"

save     ::= "save "   track " \"" name "\"" tagset?
tag      ::= "tag "    (track | person) tagset
note     ::= "note "   (track | person | "global") " \"" text "\"" until?
log      ::= "log "    track " \"" text "\"" sev?
recall   ::= "recall " person (" " digits)?
forget   ::= "forget " person

track    ::= "T" ("17" | "18")                    # ← SOLO los tracks vivos AHORA
person   ::= "P" ("4" | "7" | "12")               # ← SOLO personas que existen en la BD
tagname  ::= "residente" | "familia" | "repartidor" | "vetado" | ...   # ← vocabulario cerrado

tagset   ::= (" #" tagname)+
sev      ::= " !warning" | " !critical"
until    ::= " hasta " digits "-" digits "-" digits
name     ::= [^"\n]+
text     ::= [^"\n]+
digits   ::= [0-9]+
```

Las reglas `track`, `person` y `tagname` se **generan en cada request** a partir del estado real. El
modelo **no puede alucinar un ID de track, un ID de persona ni un tag**. No es que se valide después
de generar: es que el sampler no permite emitirlo.

La gramática compilada se cachea con clave = hash del conjunto de IDs vivos + versión del
vocabulario, para no recompilar en cada turno.

#### (c) Argumentos pre-resueltos

El modelo nunca emite un embedding, un bbox, un timestamp ni una imagen. El prompt ya lleva la tabla
de referencias cortas:

```
[escena] cam1 t=17s
 T17 desconocida 3.2s quieta "mujer con una caja"
 T18 P4(María) #familia 12s
```

Así `»save T17 "Ana" #repartidor` basta: Argus resuelve `T17` → track → mejor crop por ángulo →
embedding → filas de BD. **El modelo solo aporta la parte semántica** (el nombre, los tags), que es
lo único que solo un modelo de lenguaje puede aportar. Todo lo mecánico lo hace C++.

#### (d) Ejecución fire-and-forget

`save`, `tag`, `note` y `log` son **efectos laterales**: su resultado no cambia lo que el modelo va a
decir. Se ejecutan **mientras el modelo sigue generando la frase hablada** → latencia añadida cero.

Solo `recall` necesita ida y vuelta, y se resuelve con SQL en <1 ms inyectando **una línea** en el
contexto, no lanzando una pasada nueva de inferencia.

#### El registro de tools

```cpp
struct ToolCall { std::string verb; std::vector<std::string> args; };

struct ToolResult
{
  bool        ok{false};
  bool        needsRoundTrip{false};
  std::string line;              // solo si needsRoundTrip
};

class ToolRegistry
{
public:
  ToolResult dispatch(const ToolCall& call, const SceneContext& scene);
  std::string buildGrammar(const SceneContext& scene) const;
private:
  MemoryService     memoryService_;
  PersonRepository  personRepository_;
  EventRepository   eventRepository_;
};
```

Dispatch = búsqueda por verbo + llamada a función **en el mismo proceso**. Sin JSON-RPC, sin MCP, sin
socket, sin salto de hilo. El coste de "ejecutar un tool" es el de una consulta a SQLite.

> Notación: en el resto del documento `»verbo` es **taquigrafía** de
> `<|tool_call_start|>verbo …<|tool_call_end|>`, para no repetir los delimitadores en cada ejemplo.

| Verbo | Firma | Efecto | Round trip |
|---|---|---|---|
| `»save` | `save <T#> "<nombre>" [#tag…]` | Enrolla el rostro (§4.4) | No |
| `»tag` | `tag <T#\|P#> #tag…` | `person_tag` upsert (`source='llm'`) | No |
| `»note` | `note <T#\|P#\|global> "<hecho>" [hasta <fecha>]` | `context_note` con vigencia | No |
| `»log` | `log <T#> "<qué pasó>" [!warning\|!critical]` | `event` + `person_event` | No |
| `»recall` | `recall <P#> [N]` | Timeline compacto por SQL | **Sí** (<1 ms) |
| `»forget` | `forget <P#>` | Borra identidad y embeddings | No — **exige confirmación del Owner** |

`»forget` no se ejecuta directamente: encola una solicitud que el Owner confirma. Un modelo no debe
poder borrar identidades de forma autónoma, y el derecho al olvido merece un acto explícito.

### 4.4 Guardar el rostro "con el contexto necesario"

```
Track de persona sin identidad
  │
  ├─ cada frame: CropQuality puntúa el crop full-res
  │     · lado ≥ min_face_px          · nitidez (varianza del laplaciano) ≥ min_sharpness
  │     · yaw/pitch dentro de ±35°    · confianza del detector de cara
  │
  ├─ trinquete: retiene en RAM los 3 mejores crops, uno por ángulo
  │     (frontal / perfil-izq / perfil-der, según el yaw estimado)
  │
  └─ al emitir el LLM  »save T17 "Ana" #repartidor:
        person.create{name:"Ana"}
        para cada crop retenido:  embedding → face_embedding{angle_label, quality}
        person_tag: repartidor  (source='llm')
        person.observation ← línea inicial de contexto:
              "Vista por primera vez el 7/8 a las 14:32 en cam1 (puerta).
               Traía una caja. Etiquetada como repartidor."
        event{event_type:"person_enrolled"} + person_event{confidence}
        FaceDB::insert × 3
  → identificable desde el frame siguiente, y también de perfil
```

Los tres ángulos **no cuestan nada extra**: esos crops ya se habían calculado y puntuado por el
trinquete de §3.3. Y son necesarios: un solo embedding frontal falla en cuanto la persona gira la
cabeza. Por eso `face_embedding.angle_label` ya existe en el esquema.

### 4.5 Contexto en 4 capas con presupuesto fijo de tokens

El problema que degrada estos sistemas con el tiempo: el historial crece, el prefill crece, y al mes
6 cada respuesta tarda el triple que el primer día.

```
L0  IDENTIDAD   ~180 tok   system prompt + gramática + vocabulario de tags
                           → KV permanente, prefilado UNA VEZ en el arranque
L1  MUNDO       ~120 tok   residentes + context_notes activas + cámaras
                           → re-prefill solo cuando cambia (raro)
L2  ESCENA       ~80 tok   tracks vivos + identidades + eventos de los últimos 5 min
                           → append-only
L3  PERFIL       ~60 tok   person.observation + últimos 3 eventos + tags
                           → solo si hay persona identificada
```

**El mecanismo que lo hace rápido** — ⚠️ **ver §6.3: el mecanismo cambió tras auditar el modelo.**

Un borrador de este plan proponía `llama_memory_seq_cp` para clonar el prefijo y
`llama_memory_seq_rm` para recortar L2/L3 entre turnos. **Eso no es fiable con LFM2.5**, que es
híbrido: 10 de sus 16 bloques son convolucionales y tienen **estado recurrente**, que no se indexa por
posición y por tanto no se puede "recortar desde el token N" (§6.0).

**El mecanismo correcto es snapshot + restore** con `llama_state_seq_get_data` /
`llama_state_seq_set_data`: se prefila L0+L1 **una vez en el arranque**, se guarda el snapshot, y cada
turno lo **restaura** (un `memcpy` de un buffer contiguo) en lugar de truncar. Restaura el KV **y el
estado conv**, así que es correcto para esta arquitectura. Detalle completo en §6.3.

Lo que no cambia: el snapshot también se persiste a disco, así que **tras reiniciar el backend la
conversación arranca caliente**, sin re-prefilar nada.

Prefill por turno: ~140 tokens (L2+L3) en lugar de todo el historial → **TTFT ~80 ms**.

Esto obliga a refactorizar `LlmService` (hallazgo 0.2 nº2):

```cpp
class LlmSession                       // una por cámara / por conversación
{
public:
  explicit LlmSession(int seqId);
  drogon::Task<void> generateStream(const SessionTurn& turn, TokenCallback onToken,
                                    CancellationToken cancel);
  void  invalidateWorld();             // fuerza re-prefill de L1
  bool  saveState(const std::string& path) const;
  bool  loadState(const std::string& path);
private:
  int   seqId_;
  // ...
};
```

`LlmService` pasa de clase estática con `resetContext` a fábrica de sesiones sobre un modelo y un
contexto compartidos. **Los llamadores actuales (`labs/voice-test`, el chat HTTP) siguen funcionando
con una sesión por defecto**, así que el cambio es compatible hacia atrás.

### 4.6 Compactación del perfil: coste constante por persona

```
si count(person_event WHERE person_id = P) > compact_threshold  (p. ej. 20)
   y el sistema está idle (ni conversación ni detección activa):
       LLM: resume los eventos en ≤200 chars → person.observation
       los eventos crudos SIGUEN en la BD (recall exacto por SQL)
       marca la marca de agua de compactación en details
```

Resultado: **el coste de prompt por persona es constante, no lineal en su historia**. Es lo que hace
que el sistema siga siendo rápido al mes 6 en lugar de degradarse. Y la información exacta no se
pierde: vive en `event`, accesible por `»recall` con SQL.

### 4.7 La escalera de decisión

```
Puerta 0 — TAG            ~0 µs    lookup en person_tag (hash, indexado)
   #vetado      → evento crítico + alarma, NO se consulta ningún modelo
   #residente   → silencio (registrar y salir)
   #repartidor  → informativo si está en su ventana horaria esperada
                                                  ↓ (sin tag decisivo)
Puerta 1 — REGLAS         ~1 µs    zone_type + clase + horario + cooldown por track
   zona exclude → descartar        zona alert + desconocido → escalar
                                                  ↓ (no resuelto)
Puerta 2 — IDENTIDAD      ~30 ms   1 vez por track, con trinquete
   identificado → volver a Puerta 0 con sus tags
                                                  ↓ (desconocido)
Puerta 3 — VLM            ~800 ms  1 vez por track, solo desconocido + LINGERING
                                                  ↓
Puerta 4 — LLM + TOOLS    ~80 ms   solo si hay que hablar, decidir o recordar
```

**Objetivo medible: >95% de los eventos resueltos en las puertas 0-1**, con
`argus_decision_gate_total{gate}` para verificarlo. Si ese ratio baja, algo está mal configurado
(faltan tags, faltan zonas) y la métrica lo dice antes de que se note en la factura de CPU.

Esto es lo que separa "un asistente que sabe quién vive aquí" de "una cámara que llama a un modelo
cada vez que se mueve una hoja".

---

## 5. Eje D — Higiene del repositorio

### 5.1 `labs/` para binarios de prototipado

```
labs/
  tapo-probe/      # validación de protocolos Tapo contra hardware real (existente)
  voice-test/      # prototipado de STT/VAD/TTS/LLM (existente)
  hw-probe/        # nuevo: imprime el HardwareProfile y el tier
  detect-probe/    # nuevo: --backend/--model/--imgsz, compara backends
```

`labs` en vez de `probes` o `tools` porque su función no es solo validar hardware: es **definir
funcionalidades nuevas** antes de cablearlas al backend. Es el sitio donde un `detect-probe` decide
qué backend gana antes de que exista `DetectionPipeline`.

Cambios necesarios:
- `CMakeLists.txt:24-25` → `add_subdirectory(labs/voice-test)` / `add_subdirectory(labs/tapo-probe)`.
  Los `CMakeLists.txt` internos usan `${SRC_ROOT}` y `${CMAKE_SOURCE_DIR}`, así que **no cambian**.
- `AGENTS.md`: fila `tapo-probe/` → `labs/tapo-probe/`, y añadir `labs/` a la tabla de referencia.
- `CONTEXT.md` y `README.md`: rutas actualizadas.

### 5.2 Documentos borrados ✅ hecho

- `CAMERA_INTEGRATION_PLAN.md` — su Fase 1 está implementada; el resto se reordenó en este plan.
- `CAMERA_INTEGRATION_REVIEW.md` — registro de auditoría del anterior; quedaba huérfano.

Referencias actualizadas al borrarlos (si no, quedaban enlaces muertos en los documentos que leen los
agentes):

| Archivo | Cambio aplicado |
|---|---|
| `AGENTS.md` | Fila de `tapo/` sin referencia al plan borrado; añadidas filas `labs/`, `labs/tapo-probe/`, `labs/voice-test/` y este plan |
| `CONTEXT.md` (Eigen) | `voice-test/` → `labs/`; el tracker apunta a este plan |
| `CONTEXT.md` (voice test) | Rutas y comando de ejecución → `build/prod/labs/voice-test/` |
| `CONTEXT.md` (Tapo Fase 1) | "Implements Phase 1 of …" → redactado como hecho consumado |
| `CONTEXT.md` (Anexo A) | La nota *"Deviation from the plan's Annex A"* se reescribió como **hecho autónomo** sobre la derivación de `hashedKey` (§5.3) |

### 5.3 Un detalle que no se debe perder al borrar el plan

El plan borrado tenía un **error real en su Anexo A** que quien implementó la Fase 1 detectó y
corrigió. Merece quedar documentado porque su modo de fallo es engañoso:

```
INCORRECTO (borrador del plan):  lsk = sha256("lsk" + cnonce + nonce + hashedPassword)[:16]
CORRECTO   (pytapo, y el código): hashedKey = sha256(cnonce + hashedPassword + nonce)
                                  lsk       = sha256("lsk" + cnonce + nonce + hashedKey)[:16]
```

`hashedKey` **no es** `hashedPassword`: es el mismo valor que valida `device_confirm`. Con la versión
incorrecta **el handshake autentica bien** y luego **falla al descifrar todas las respuestas** — se
parece a un login correcto seguido de payloads basura, que es de los síntomas más difíciles de
diagnosticar. La versión correcta está en `CONTEXT.md` y en
`secure-passthrough-transport.cc`.

---

## 6. Eje E — LLM y VLM: optimización interna

Auditoría de `src/shared/services/llm/` y `src/shared/services/vision/`. Todo lo de esta sección está
verificado leyendo el código y el header del pin `b6565`; nada es especulativo.

### 6.0 El modelo no es lo que el código asume: LFM2.5 es **híbrido**

Leyendo los tensores del GGUF (`LFM2.5-1.2B-Instruct-Q4_K_M.gguf`):

```
16 bloques en total
 ├─  6 con atención   (blk.N.attn_q_norm / attn_k_norm / attn_output)
 └─ 10 solo conv      (blk.N.shortconv.conv / in_proj / out_proj)
```

**Esto cambia tres decisiones**, y es la razón de que varias cosas del código actual sean malos
trades sin que se note:

1. **Solo 6 de 16 capas tienen KV cache.** Cuantizarlo agresivamente ahorra muy poca memoria.
2. **Las 10 capas conv tienen estado recurrente.** El estado conv **no se puede truncar por
   posición**: no es una tabla indexada por token, es un estado que evoluciona. Por eso existe
   `llama_memory_can_shift()`.
3. La plantilla de chat del GGUF **es ChatML** (verificado: `{{- "<|im_start|>" + message["role"] ... }}`),
   así que el `buildPrompt` hardcodeado **acierta hoy**. No es un bug actual, es una bomba de relojería
   para el día que se cambie de modelo.

### 6.1 LLM — bugs de calidad (arreglarlos no cuesta rendimiento)

| # | Problema | Evidencia | Arreglo |
|---|---|---|---|
| 1 | **`penalty_freq = 1.2`** en `llama_sampler_init_penalties(64, 1.1f, 1.2f, 0.0f)` | El header documenta `penalty_freq // 0.0 = disabled`. El rango sano es 0.0-0.7. **1.2 por ocurrencia distorsiona la distribución de forma brutal**, suprimiendo palabras función (artículos, preposiciones) y produciendo texto artificialmente "rebuscado" | `penalty_freq = 0.0`, `penalty_repeat = 1.1` (ese sí está bien). Si hace falta anti-repetición, subir `penalty_repeat` a 1.15, no `freq` |
| 2 | **Orden de la cadena de samplers** | Actual: `temp → top_k → top_p → penalties → dist`. El orden canónico documentado en `llama.h:1086-1092` es `top_k → top_p → temp → dist`, con penalties **antes** de las truncaciones | `penalties → top_k → top_p → temp → dist`. Aplicar `temp` antes de `top_p` cambia el significado de `top_p` (opera sobre probabilidades ya ablandadas); aplicar penalties **después** de truncar las deja casi sin efecto |
| 3 | **KV cache a `Q4_0`** (`type_k = type_v = GGML_TYPE_Q4_0`) | Solo 6 de 16 capas tienen KV (§6.0). El ahorro es pequeño y cae **precisamente sobre las capas responsables del recall a distancia** | `type_k = type_v = GGML_TYPE_F16`, o `Q8_0` si la memoria aprieta. Es el cambio de mayor impacto en calidad de todo el archivo |
| 4 | **`flash_attn_type = AUTO` + V cuantizado** | La cuantización del V-cache en llama.cpp normalmente **requiere** flash attention. Con `AUTO` en CPU puede resolverse a desactivada, dejando una combinación no soportada | Al pasar a F16 el problema desaparece. Si se quiere `Q8_0`, activar FA explícitamente y **verificar en el log** que quedó activa |
| 5 | **Seed fija `llama_sampler_init_dist(42)`** | Salida idéntica para el mismo prompt. Con caché de prefijo entre turnos, produce frases calcadas | `LLAMA_DEFAULT_SEED`, o configurable con 42 solo en tests |

### 6.2 LLM — rendimiento

| # | Problema | Coste real | Arreglo |
|---|---|---|---|
| 1 | **`resetContext = true` por defecto** | **Re-prefill completo del historial en cada turno.** Es la causa principal de la lentitud percibida | Caché de prefijo (§6.3) |
| 2 | 🐞 **El prefill mete `nTokens` en UN batch** (`llama_batch_init(nTokens, 0, 1)`) pero `n_batch = 1024` | Un prompt de >1024 tokens hace **fallar `llama_decode`** → *"prompt decode failed"* y respuesta vacía. Con `context_size = 32768` y el sistema de memoria, **esto va a ocurrir** | Trocear el prefill en trozos de `n_batch`, con `logits` solo en el último token del último trozo |
| 3 | **`llama_batch_init` + `llama_batch_free` 2× por request** | `AGENTS.md` 13d afirma *"LLM also reuses a single `llama_batch` per generation loop"* — **el código no lo hace**. Documentación y realidad divergen | Dos batches persistentes reutilizados (uno de prefill dimensionado a `n_batch`, uno de decode de 1 token) |
| 4 | **`promptTokens(promptLen * 2)`** | Un token ≈ 3-4 chars, así que reserva ~8× lo necesario. Un prompt de 8 KB reserva 64 KB de tokens | `llama_tokenize` con buffer nulo devuelve el conteo exacto (negativo); o dimensionar a `promptLen/3 + 16` |
| 5 | **`n_gpu_layers = 0` hardcodeado** | Nunca hay offload, ni en máquinas con dGPU | Derivar de `HardwareProfile` (§2.1) + `llama-cpp/*:with_vulkan` en build (§2.2) |
| 6 | **`swa_full = true`** | Reserva el KV completo en lugar de solo la ventana | Poner `false` y medir; solo hace falta `true` si se reutilizan posiciones arbitrarias |
| 7 | **`queueInLoop` por token** en la ruta async | Una tarea de event loop + copia de `std::string` + copia de lambda **por token** | Aceptable a 36 tok/s; si se mide overhead, agrupar en lotes de 3-4 tokens (sin perder sensación de streaming) |
| 8 | **`warmup()` solo calienta el prefill** | El primer token generado paga la inicialización de la ruta de decode de 1 token | Añadir un `llama_decode` de 1 token tras el prefill de calentamiento |

### 6.3 LLM — caché de prefijo, adaptada a un modelo híbrido

⚠️ **Corrección a §4.5 de este plan.** Ahí escribí que el mecanismo sería
`llama_memory_seq_rm` + `llama_memory_seq_cp`. **Con 10 capas conv recurrentes eso no es fiable**: el
estado conv no se indexa por posición, así que truncar "desde el token N" no tiene un significado
bien definido, y `llama_memory_can_shift()` existe precisamente para reportar esa limitación.

**El mecanismo correcto para este modelo es snapshot + restore:**

```
Arranque (una vez):
   prefill de L0 (system + gramática + vocabulario de tags) + L1 (mundo)
   snapshot = llama_state_seq_get_data(ctx, seq0)        → a RAM y a disco
   → coste pagado UNA vez en toda la vida del proceso

Cada turno:
   llama_state_seq_set_data(ctx, snapshot, seq0)         → restaura KV **y estado conv**
   decode solo de L2 (escena) + L3 (perfil) + turno      ≈ 140 tokens
   generar

Al cambiar el mundo (nuevo residente, context_note nueva):
   invalidar el snapshot y recomputarlo (raro)
```

Restaurar un snapshot es un `memcpy` de un buffer contiguo — mucho más barato que re-prefilar
cientos de tokens, y **es correcto para el estado conv**, que `seq_rm` no puede garantizar.

`llama_state_seq_get_data` / `set_data` también dan la persistencia entre reinicios que ya estaba en
el plan: el snapshot de L0+L1 se guarda en disco y el arranque no re-prefila nada.

**Para el chat multi-turno libre** (donde el historial crece de verdad y no hay capas fijas), el
snapshot se toma **por turno**: tras generar, se guarda el estado; en el turno siguiente se restaura y
solo se decodifica el mensaje nuevo. Eso convierte un chat de N turnos de O(N²) tokens prefilados a
O(N).

### 6.4 LLM — quitar el contexto estático

Petición explícita: el servicio debe **habilitar un chat con el LLM**, nada más. Hoy `buildPrompt`
inyecta un `SYSTEM_PROMPT` de 9 líneas hardcodeado en el header cuando el llamador no manda ninguno.

Problemas de que esté ahí:
- **Contamina cada llamada** con instrucciones de "asistente de seguridad" incluso cuando la llamada
  es un resumen de perfil (§4.6) o una descripción de escena — tareas donde ese prompt empuja al
  modelo en la dirección equivocada.
- Son ~130 tokens de prefill pagados en cada llamada que no lo necesita.
- El *quién es Argus* pertenece a la capa L0 del `SceneContext` (§4.5), que es quien sabe qué
  personalidad y qué tools hay disponibles. Tenerlo en dos sitios garantiza que se desincronicen.

**Cambio:**

```cpp
// llm-service.hxx — se elimina por completo:
//   static constexpr const char* SYSTEM_PROMPT = R"SYSPROMPT(...)SYSPROMPT";

// buildPrompt: sin inyección implícita. Si el llamador no manda system, no hay system.
// La plantilla se lee del modelo, no se hardcodea:
const char* tmpl = llama_model_chat_template(model_.get(), nullptr);
// → llama_chat_apply_template(tmpl, msgs, n, add_assistant, buf, len)
```

Dos avisos sobre `llama_chat_apply_template`, que el header documenta: **no usa un parser jinja**,
solo reconoce una lista predefinida de plantillas. Para LFM2.5 (ChatML) funciona. La estrategia
segura es: intentar la plantilla del modelo, y si `llama_chat_apply_template` devuelve error, caer al
constructor ChatML actual y **avisar por log** — así el servicio nunca queda peor que hoy.

**Impacto en llamadores: uno solo.** `labs/voice-test/voice-test.cc:296` construye `ChatRequest` con
`state.history` y no manda system. Hay que añadirle su propio system prompt (el de un asistente de
voz, que es lo que es). Es el único sitio que cambia.

### 6.5 VLM — el problema dominante: el KV cache se copia entero en cada token

```cpp
auto copyPast = [&](const std::vector<Ort::Value>& outs, int outIdx, int l, bool key) {
  ...
  dst.assign(outs[outIdx].GetTensorData<float>(), ... + sh[0]*sh[1]*sh[2]*sh[3]);
};
for (int l = 0; l < kNumLayers; ++l) {          // 32 capas
  copyPast(outs, 1 + 2 * l, l, true);           // K
  copyPast(outs, 2 + 2 * l, l, false);          // V
}
```

Esto corre **después de cada token generado**. Aritmética con los valores del propio archivo
(`kNumKvHeads = 5`, `kHeadDim = 64`, prompt de 80 posiciones):

```
por tensor:  5 × 80 × 64 floats × 4 B  ≈ 102 KB
por token:   × 64 tensores (32 capas × K y V)  ≈ 6.5 MB copiados
y crece con cada token generado
64 tokens de caption  →  cientos de MB de memcpy puro
```

**Arreglo**: ONNX Runtime permite alimentar como entrada un `Ort::Value` que fue **salida** de un
`Run` anterior. Se conserva el `std::vector<Ort::Value>` del paso previo y se pasan sus
`present.N.key`/`present.N.value` directamente como `past_key_values.N.key`/`value` del paso
siguiente. **Cero copias**, y desaparecen `pastKeys`, `pastValues`, `pastShapes` y `copyPast`.

Alternativa si hiciera falta control fino: `Ort::IoBinding` con buffers pre-asignados al tamaño
máximo (`promptLen + maxTokens`) y una vista creciente. Más código, mismo resultado.

### 6.6 VLM — el resto, por orden de impacto

| # | Problema | Coste | Arreglo |
|---|---|---|---|
| 1 | **Los embeddings del prompt se recalculan en cada `describe()`** | `promptIds()` es **estático** (un `static const std::vector` fijo) → sus embeddings son **constantes**, y sin embargo se lanza un `Run` completo de `embedTokens_` sobre 80 tokens en cada llamada | Calcularlos **una vez en `init()`** y guardarlos. Por llamada solo queda el `memcpy` de las 64 filas de imagen. **Elimina un `Run` de ORT por descripción** |
| 2 | **El hash FNV-1a recorre 3.1 MB byte a byte** | `pixels.size() * sizeof(float)` = 786 432 × 4 B, con un XOR + multiplicación **por byte**: ~3 M iteraciones escalares. Y se paga **siempre**, incluso cuando la caché acierta — y encima **después** de haber hecho ya la conversión a float | Reordenar: `resize` → **hashear el `cv::Mat` de 8 bits** (786 KB, 4× menos datos) → si acierta, **saltarse también la conversión a float**. Y leer de 8 en 8 bytes en lugar de 1 en 1 |
| 3 | **`INTER_LANCZOS4` para reducir** 2688×1520 → 512×512 | Lanczos es el interpolador más caro de OpenCV **y está pensado para ampliar**. Al reducir sin filtro previo introduce aliasing | **`INTER_AREA`**: más rápido **y de mejor calidad al reducir** (promedia el área fuente). Gana en las dos dimensiones a la vez |
| 4 | **`preprocess` hace un triple bucle escalar** | `pixels[idx++] = (row[x*3+c] * scale - mean) * invStd` recorre la imagen **3 veces** (una por canal) con acceso strided `x*3+c` → localidad de caché mala en 786 k iteraciones | Como `kMean = kStd = 0.5`, la transformación es exactamente `v/127.5 - 1`: un solo `convertTo(CV_32F, 1.0/127.5, -1.0)` + `cv::split` en planos (o `cv::dnn::blobFromImage`). Vectorizado por SIMD |
| 5 | **`embed(token)` lanza un `Run` de ORT por token generado** | Un dispatch completo de ORT para lo que es un *gather* en una tabla | Caché LRU de embeddings de token (4096 × 960 floats ≈ 15 MB). Los captions reutilizan tokens comunes constantemente |
| 6 | **Decodificación greedy sin anti-repetición** | `pickBest` es argmax puro. SmolVLM es propenso a bucles del tipo *"a man a man a man"* | `no_repeat_ngram_size = 3` (barato: un set de trigramas ya emitidos) o una penalización leve. **Es una mejora de calidad, no de velocidad** |
| 7 | **Sin `Ort::RunOptions::SetTerminate()`** | El VLM no se puede cancelar → el barge-in del §9 (Fase H) desperdicia cientos de ms de `Run` | Guardar el `Ort::RunOptions` de la corrida en curso y exponer `cancel()` |
| 8 | **`namesStorage` `static` dentro del bucle** | Se declara `static` pero **se reasigna en cada llamada** (`namesStorage[2*l] = "present." + ...`): 64 construcciones de `std::string` por descripción | Construir los 65 nombres **una vez en `init()`** como miembro |
| 9 | **`spin_duration_us = 1000`** | ORT quema CPU haciendo spin-wait entre operaciones. Baja latencia cuando el VLM corre solo; **contraproducente** cuando compite con LLM y TTS | Derivar de `HardwareProfile`: spin en máquinas con muchos cores, `0` en las de pocos |
| 10 | **Logits de prefill para todas las posiciones** | El decoder devuelve logits de las 80 posiciones (80 × 49 280 floats ≈ **15.7 MB**) cuando solo se necesita la última | *(verificar)* Si el grafo ONNX se puede recortar para aplicar la LM head solo a la última posición, el prefill baja de forma notable. Requiere reexportar el modelo — evaluar antes de comprometerse |
| 11 | **`kNumLayers = 32`, `kNumKvHeads = 5`, `kHeadDim = 64`, `kEmbedDim = 960`, `kEosId = 49279` hardcodeados** | Cambiar de modelo o de export produce corrupción silenciosa, no un error | Leerlos de las formas del grafo (`GetInputTypeInfo` / `GetOutputTypeInfo`) en `init()` y **verificar** contra las constantes, avisando si difieren |

### 6.6b VLM: migración a LFM2.5-VL-450M vía llama.cpp vendorizado (medido)

**Decisión tomada: llama.cpp sale de Conan y entra como submódulo** en
`third_party/llama.cpp` (tag `b10305`), con `LLAMA_BUILD_MTMD=ON`. Motivos, todos verificados:

| Motivo | Evidencia |
|---|---|
| `llama-cpp/b6565` es la recipe **más nueva** de Conan Center | `conan search llama-cpp/*` → b2038…b6565 |
| Su proyector `lfm2` exige `mm.input_norm.*`, que LFM2.5-VL eliminó | `clip_init: unable to find tensor mm.input_norm.weight`. El fix es `#18594` (2026-01-04) |
| El tiling correcto de LFM2-VL llegó en `#19454` (2026-02-09) | ambos commits están en b10305 |
| Conan compila **solo CPU** y no expone componente mtmd | el paquete trae `libmtmd.a` pero ninguna recipe lo declara |
| Reimplementar `Lfm2VlImageProcessorFast` por la vía ONNX es riesgo de **degradación silenciosa** | tiling dinámico, `min_tiles=2`, `max_tiles=10`, thumbnail, row/col info |

**Cambios de API que costó b6565 → b10305** (visibles en compilación, el buen tipo de fallo):

```
llama_model_params.use_mmap   → .load_mode = LLAMA_LOAD_MODE_MMAP
llama_sampler_init_penalties(...) gana n_vocab como PRIMER argumento
mtmd_context_params pierde .verbosity, gana .image_min_tokens/.image_max_tokens
mtmd_input_text gana .text_len  ← si no se rellena, el texto se ve VACÍO
mtmd_decode_use_non_causal(ctx) → (ctx, chunk)
```

**Comparación medida** (mejor de 3, 48 tokens generados, misma imagen sintética):

| Entrada | SmolVLM2-500M ONNX | LFM2-VL-450M | **LFM2.5-VL-450M** |
|---|---|---|---|
| 256×256 | n/a (siempre 512) | 761 ms | **721 ms** (83 tok) |
| 384×384 | n/a | 1 067 ms | **1 031 ms** |
| 512×512 | ~946 ms | 1 602 ms | 1 453 ms |
| 1280×720 | ~946 ms | 7 034 ms | 3 121 ms |
| 2688×1520 | ~1 553 ms | 9 021 ms | 8 324 ms |
| generación | 46-48 tok/s | 75-85 tok/s | **78-91 tok/s** |
| init | 845 ms | 225 ms | **235 ms** |
| peak RSS | ~1,0 GB | 1,56 GB | **0,78 GB** |
| disco | 492 MB | 547 MB | 568 MB |

**Precisión sobre la misma imagen** (gradiente verde-azul, cuadrado blanco izq., círculo rojo dcha.,
texto "ARGUS"):

| Modelo | Caption | |
|---|---|---|
| SmolVLM2 | *"a logo… gradient blue to green, with the **'AR' in the center**"* | parcialmente erróneo |
| LFM2-VL | *"a screenshot of a **smartphone's home screen**"* | erróneo |
| **LFM2.5-VL** | *"gradient green and blue, **white square on the left**, **red circle on the right**, the word **'ARGUS'** in white"* | **correcto** |

**Cómo se controla el coste**: `image_max_tokens` reduce el detalle **por tile** pero no colapsa el
número de tiles — a 720p el prompt sigue en 593-785 tokens, y por debajo de 256 tokens **el modelo
deja de leer el texto**. El coste se controla con la **resolución de entrada**, que encaja con §13.2:
el VLM recibe el **crop de la persona**, no el frame completo. Un crop a 256-384 px cuesta
721-1 031 ms con la precisión de LFM2.5.

**Resultado neto en el punto de operación real** (crop de 256 px): **-24% de latencia, ~1,8× de
velocidad de generación, -22% de RAM, -72% de tiempo de arranque, precisión claramente superior y
prompts arbitrarios** (`"¿Hay una persona? sí/no"` → `"No."`), frente a un `VisionRequest::prompt` que
hoy es código muerto. Y permite borrar las ~700 líneas de pipeline ONNX hecha a mano.

**Vulkan**: el submódulo activa `GGML_VULKAN` automáticamente si hay Vulkan + `glslc` +
**SPIRV-Headers**; si falta alguno, degrada a CPU con un aviso. `setup.sh` instala los tres.

### 6.7 Resultado esperado

| Métrica | Antes | Después | De dónde sale |
|---|---|---|---|
| LLM, TTFT en turno N>1 | re-prefill completo del historial | **~80 ms** | Snapshot + restore (§6.3) |
| LLM, prompts >1024 tokens | **fallan** | funcionan | Prefill troceado (§6.2 #2) |
| LLM, calidad de texto | distorsionada por `penalty_freq=1.2` y orden de samplers | correcta | §6.1 #1, #2 |
| LLM, recall a distancia | degradado por KV en `Q4_0` | correcto | §6.1 #3 |
| VLM, memcpy por caption | **cientos de MB** | **~0** | `Ort::Value` reencadenados (§6.5) |
| VLM, `Run`s de ORT por caption | 1 encoder + 1 prompt-embed + N token-embed + N+1 decoder | 1 encoder + **0** prompt-embed + ~0 token-embed + N+1 decoder | §6.6 #1, #5 |
| VLM, preprocesado | Lanczos + triple bucle escalar + hash de 3.1 MB | `INTER_AREA` + `convertTo` SIMD + hash de 786 KB | §6.6 #2, #3, #4 |
| VLM, cancelable | no | sí | §6.6 #7 |

Ninguno de estos cambios sacrifica calidad. Cuatro de ellos (**`penalty_freq`**, **orden de
samplers**, **KV en F16**, **`INTER_AREA`**) la **mejoran** mientras además reducen o mantienen el
coste — son los que hay que hacer primero por relación beneficio/riesgo.

---

## 7. Componentes a implementar

### 6.1 Hardware y adaptación

| Archivo | Contenido |
|---|---|
| `src/shared/wrapper/hardware-profile/hardware-profile.hxx/.cc` | `HardwareProfile`, `HardwareProbe::get()`, derivación de `CapabilityTier`, `toJson()` |
| `src/shared/wrapper/thread-budget/` (modificar) | Consultar `HardwareProfile` para GPU/ISA; mantener la API actual intacta (regla 13b) |
| `src/shared/services/config-service/` (modificar) | Resolución de valores por tier: `getTiered<T>(key)` con override explícito en `config.toml` |
| `scripts/setup.sh` (modificar) | `detect_hardware()` → `scripts/.hw-profile` → opciones de Conan; descarga por manifest |
| `models/manifest.toml` | Modelos por tier |
| `labs/hw-probe/` | Binario que imprime el perfil, el tier y la configuración resuelta |

### 6.2 Detección, tracking y ruta de frames

| Archivo | Contenido |
|---|---|
| `src/shared/services/detector/detector-backend.hxx` | `IDetectBackend` + `struct Detection` |
| `src/shared/services/detector/ncnn-yolo-backend.hxx/.cc` | Primario: ncnn+Vulkan, `(1,84,8400)` + NMS, `vkcache` persistido |
| `src/shared/services/detector/ort-yolo-backend.hxx/.cc` | Fallback y oráculo de tests: ORT-CPU, `(1,300,6)` |
| `src/shared/services/tracker/byte-tracker.hxx/.cc` | ByteTrack: asociación en 2 pasadas, `cv::KalmanFilter`, `struct Track` |
| `src/shared/services/stream/video-decoder.hxx/.cc` | Decodificación acelerada, **salida NV12**, reconexión, métricas |
| `src/shared/services/camera-frame/frame-processor.hxx/.cc` | **Gate sobre plano Y con stride** + `warpAffine` único a tensor + mapeo bbox→full-res + crop |
| `src/shared/services/camera-frame/crop-quality.hxx/.cc` | Puntuación de crop + trinquete de los 3 mejores por ángulo |
| `src/shared/services/detection-pipeline/detection-pipeline.hxx/.cc` | Orquestación + backpressure + degradación reactiva |
| `labs/detect-probe/` | `--backend --model --imgsz --tier`, imprime ms por etapa |

### 6.3 Memoria de personas

| Archivo | Contenido |
|---|---|
| `database/schema.sql` (modificar) | `person_tag` + índice |
| `src/shared/enums.hxx` (modificar) | `enum class TagSource` (regla 1) |
| `src/shared/repositories/person-tag/` | `person-tag-query.hxx` + repositorio (regla 3) |
| `src/shared/services/memory/tag-vocabulary.hxx` | Vocabulario cerrado — fuente única para enum, gramática y reglas |
| `src/shared/services/memory/memory-service.hxx/.cc` | Enrolamiento, tags, timeline por persona, compactación de perfil |
| `src/shared/services/memory/scene-context.hxx/.cc` | Ensamblado de L0-L3 con presupuesto de tokens |
| `src/shared/services/memory/tool-registry.hxx/.cc` | `dispatch()` + `buildGrammar()` + fire-and-forget |
| `src/shared/services/memory/tool-parser.hxx/.cc` | Parser del DSL (trivial: la gramática garantiza la forma) |
| `src/shared/services/rules/rule-engine.hxx/.cc` | Escalera de decisión (puertas 0-1) + métricas por puerta |
| `src/shared/services/llm/llm-session.hxx/.cc` | Sesión con `seq_id`, snapshot/restore del prefijo (§6.3), gramática perezosa |
| `src/shared/services/face/face-service.*` (modificar) | `extractMat` / `identifyMat` / `identifyMatAsync` (§3.2) |

### 6.5b LLM y VLM — trabajo del Eje E

| Archivo | Cambios |
|---|---|
| `src/shared/services/llm/llm-service.hxx` | **Eliminar `SYSTEM_PROMPT`** (§6.4). `ChatRequest` gana `systemPrompt` opcional y `reusePrefix`. De clase estática a fábrica de sesiones, compatible hacia atrás |
| `src/shared/services/llm/llm-service.cc` | `penalty_freq → 0.0` · reordenar la cadena de samplers · `type_k/type_v → F16` · prefill troceado a `n_batch` · batches persistentes · plantilla vía `llama_model_chat_template` con fallback a ChatML · `n_gpu_layers` desde `HardwareProfile` · `swa_full = false` · seed configurable · warmup de la ruta de decode |
| `src/shared/services/llm/prefix-cache.hxx/.cc` | Snapshot/restore con `llama_state_seq_get_data`/`set_data`; comprobación de `llama_memory_can_shift()`; persistencia a disco |
| `src/shared/services/vision/vision-service.cc` | **Reencadenar los `Ort::Value` de KV** (§6.5) · embeddings de prompt precalculados en `init()` · `INTER_AREA` · `convertTo`+`split` SIMD · hash sobre el Mat de 8 bits antes de la conversión · LRU de embeddings de token · `no_repeat_ngram = 3` · `SetTerminate()` · nombres de tensores construidos una vez · dimensiones leídas del grafo |
| `src/shared/services/vision/vision-service.hxx` | Entrada por `cv::Mat` (§3.2); `cancel()`; miembros de caché de prompt y de nombres |
| `labs/voice-test/voice-test.cc` | Pasar su propio system prompt (único llamador afectado por §6.4) |
| `labs/llm-bench/` | Nuevo: mide TTFT, tok/s, prefill troceado y snapshot/restore antes y después. Sin esto los números de §6.7 no son verificables |

### 6.4 Observabilidad

| Métrica | Para qué |
|---|---|
| `argus_hw_tier` | Qué tier se resolvió |
| `argus_decode_ms{camera}` · `argus_frame_to_tensor_ms` | Validar la ruta de 3 pasadas (§3.1) |
| `argus_detector_ms{backend}` | Elegir backend y alimentar la degradación reactiva |
| `argus_motion_gate_skipped_ratio` | Validar que el gate ahorra de verdad |
| `argus_frames_dropped_total{camera,stage}` | Backpressure |
| **`argus_decision_gate_total{gate}`** | **El >95% en puertas 0-1 (§4.7)** |
| `argus_face_crops_rejected_total{reason}` | Calibrar `CropQuality` |
| `argus_llm_ttft_ms` · `argus_llm_prefill_tokens` | Validar el `seq_cp` del prefijo |
| `argus_tool_calls_total{verb,ok}` · `argus_tool_parse_failures_total` | **Debe quedarse en 0** |
| `argus_person_profile_tokens` | Validar que la compactación mantiene el coste constante |

---

## 8. Configuración

Los valores por defecto los resuelve el **tier**; `config.toml` solo declara overrides explícitos.
Nada de números de hilos (regla 13b).

```toml
[hardware]
tier = "auto"                # "auto" | "minimal" | "low" | "balanced" | "high"
# Overrides finos para diagnóstico; vacío = lo decide el tier.
force_detector_backend = ""  # "" | "ncnn_vulkan" | "ort_cpu"
force_video_accel      = ""  # "" | "vaapi" | "nvdec" | "qsv" | "none"

[detector]
# input_size, analysis_fps y modelo salen del tier. Estos son PISOS, no palancas:
conf_person  = 0.30
conf_default = 0.45
class_watchlist = ["person", "vehicle", "dog", "cat"]
min_track_hits  = 3          # ver la nota de latencia de evento más abajo

[tracker]
max_age_frames   = 30
iou_threshold    = 0.3
linger_seconds   = 3.0
linger_max_speed = 0.02

[face]
# PISOS: no escalan con el tier bajo ninguna circunstancia.
crop_from_fullres = true
crop_padding_pct  = 20
min_face_px       = 64
min_sharpness     = 45.0
max_face_attempts = 5
quality_ratchet_margin = 0.05   # margen para re-embeber y reemplazar
enroll_angles     = 3           # frontal + 2 perfiles

[memory]
compact_threshold     = 20      # eventos por persona antes de compactar el perfil
profile_max_chars     = 200
scene_window_seconds  = 300     # ventana de L2
recall_default_limit  = 5
event_flush_ms        = 250     # agrupación de escrituras (§3.4)

[llm]
model_path    = ""              # vacío = lo decide el tier
context_size  = 32768
# Muestreo — orden canónico: penalties -> top_k -> top_p -> temp -> dist (§6.1)
temperature      = 0.3
top_k            = 20
top_p            = 0.8
penalty_last_n   = 64
penalty_repeat   = 1.10
penalty_freq     = 0.0          # era 1.2: el header documenta "0.0 = disabled" (§6.1 #1)
penalty_present  = 0.0
seed             = 0            # 0 = LLAMA_DEFAULT_SEED; fijar solo en tests
# KV cache: F16 porque solo 6 de 16 capas de LFM2.5 tienen atención (§6.0, §6.1 #3)
kv_type          = "f16"        # "f16" | "q8_0"
flash_attn       = "auto"       # "auto" | "on" | "off"
swa_full         = false
# El chat template se lee del GGUF; ChatML solo como fallback (§6.4)
chat_template    = "model"      # "model" | "chatml"
reuse_prefix        = true      # snapshot + restore (§6.3)
persist_session     = true      # llama_state_seq_* a disco
tools_enabled       = true
tool_trigger        = "<|tool_call_start|>"   # token nativo del modelo, no inventado
grammar_cache_size  = 8

[vision]
image_size        = 512         # fijado por el encoder SigLIP
max_tokens        = 64
resize_interp     = "area"      # era lanczos4: peor y más lento al reducir (§6.6 #3)
no_repeat_ngram   = 3           # evita bucles del tipo "a man a man" (§6.6 #6)
frame_cache_slots = 8           # eran 2
token_embed_cache = 4096        # LRU de embeddings de token (§6.6 #5)
# spin_duration_us lo decide HardwareProfile: quemar CPU esperando solo compensa
# en máquinas con cores de sobra (§6.6 #9).

[events]
retention_days = 90
syncable       = true
```

> **Nota de latencia de evento.** Con `min_track_hits = 3` y `analysis_fps = 8`, confirmar un track
> cuesta 250 ms **solo en esperar frames**. El objetivo realista es: *detección disponible* < 100 ms
> y *evento ENTER confirmado* < 450 ms. `min_track_hits = 2` baja el evento a ~300 ms a cambio de más
> falsos positivos. Para un evento de seguridad, 450 ms con un falso positivo raro es mejor negocio
> que 250 ms con ruido. Es calibración, no limitación (§11.5).

---

## 9. Fases

Cada fase tiene una **puerta**: un criterio medible que hay que cerrar antes de avanzar.

### Fase A — Perfilado de hardware
1. `HardwareProfile` + `HardwareProbe` (CPU, ISA, RAM, Vulkan vía ncnn, video accel vía libav).
2. Derivación de `CapabilityTier` + `ConfigService::getTiered()`.
3. `labs/hw-probe/`.
4. `setup.sh detect_hardware()` → opciones de Conan + descarga por manifest.

**Puerta**: `hw-probe` reporta el perfil correcto en al menos dos máquinas distintas (una con GPU,
una sin), y `setup.sh` elige las opciones de Conan coherentes en ambas.

### Fase B — Higiene y esquema
1. Mover `tapo-probe/` y `voice-test/` a `labs/`; actualizar `CMakeLists.txt`.
2. Borrar los dos documentos antiguos; actualizar `AGENTS.md:407`, `CONTEXT.md:45`, `CONTEXT.md:157`.
3. `person_tag` + `TagSource` + repositorio + vocabulario de tags.

**Puerta**: `cmake --preset dev && cmake --build --preset dev -j 8` con **0 errores, 0 warnings**
(exigencia de `AGENTS.md`).

### Fase B2 — LLM y VLM: arreglos de calidad y coste *(independiente, puede ir en paralelo a A-C)*

Va temprano porque **no depende de nada** y cuatro de sus cambios mejoran la calidad *reduciendo* el
coste. Es el mejor ratio beneficio/riesgo del plan.

1. `labs/llm-bench/` primero — sin línea base, "optimizamos" a ciegas.
2. LLM calidad: `penalty_freq → 0.0`, orden de samplers, `kv_type = f16`, seed configurable (§6.1).
3. LLM: **eliminar `SYSTEM_PROMPT`** + plantilla desde el GGUF + actualizar `labs/voice-test` (§6.4).
4. LLM: prefill troceado a `n_batch` (arregla el fallo con prompts >1024 tokens), batches
   persistentes, tokenización dimensionada (§6.2).
5. VLM: reencadenar los `Ort::Value` de KV, embeddings de prompt precalculados, `INTER_AREA`,
   `convertTo`+`split`, hash sobre 8 bits, LRU de embeddings, `no_repeat_ngram`, `SetTerminate()` (§6.5, §6.6).

**Puerta**: `llm-bench` muestra un prompt de 2000 tokens que **antes fallaba y ahora responde**;
tok/s igual o mejor; caption del VLM sin regresión de contenido en 20 imágenes de prueba y con
**memcpy por token ≈ 0** (medible con `/usr/bin/time -v` o un contador propio).

### Fase C — Detector adaptativo
1. `IDetectBackend` + `NcnnYoloBackend` (Vulkan, `vkcache`) + `OrtYoloBackend`.
2. `labs/detect-probe/` con `--backend --model --imgsz --tier`.

**Puerta**: tabla de ms por backend y por `input_size`; **las salidas de los dos backends concuerdan**
sobre las mismas imágenes. Cierra la decisión de backend con datos, no con preferencia.

### Fase D — Ruta de vídeo de 3 pasadas
1. `VideoDecoder` con salida NV12 y aceleración según perfil.
2. Gate sobre plano Y con stride; `warpAffine` único al tensor.
3. Backpressure + degradación reactiva.

**Puerta**: `argus_frame_to_tensor_ms` y `argus_decode_ms` medidos; el gate descarta >90% de los
frames en una escena estática real.

### Fase E — Tracking e identidad
1. `ByteTracker` + reglas por track.
2. `FaceService::identifyMat` (borra el encode/decode) + `CropQuality` + trinquete de 3 ángulos.

**Puerta**: un solo `trackId` durante un recorrido completo; reconocimiento correcto a **≥4 m** y
**de perfil**, comparado contra la línea base a 640 px (que debe fallar).

### Fase F — Memoria y escalera de decisión
1. `MemoryService` (enrolamiento, tags, timeline, compactación).
2. `RuleEngine` con las puertas 0-1 + `argus_decision_gate_total`.
3. `SceneContext` (L0-L3).

**Puerta**: **>95% de los eventos resueltos en las puertas 0-1** durante una jornada real.

### Fase G — Tools y sesiones de LLM
1. `PrefixCache` con snapshot/restore (§6.3) + `LlmSession` + persistencia a disco.
2. `ToolRegistry` + `buildGrammar()` + gramática perezosa con `<|tool_call_start|>` + dispatch
   fire-and-forget.

**Puerta**: TTFT < 150 ms en el segundo turno; **0 fallos de parseo en 200 tool calls**
(`argus_tool_parse_failures_total == 0`); medición real del ahorro de tokens vs JSON.

### Fase H — Conversación en streaming y barge-in
Lo ya planificado: `CancellationToken` en LLM/TTS/VLM/STT/envío 8800, `SentenceAccumulator` con
chunking asimétrico, `tts_lookahead = 1`, gate anti-eco, audio del móvil como entrada primaria.

**Puerta**: el altavoz calla en **< 100 ms** tras la interrupción.

---

## 9b. Plan de ejecución de las fases C-H

Estado al escribir esto: A (HardwareProfile), B (labs/ + higiene) y B2 (LLM/VLM)
**hechas y medidas**. Lo que sigue es el pipeline de cámara. Orden fijado por
dependencias reales, no por comodidad.

### C0 — Cerrar los huecos del detector de hardware *(prerrequisito, ~1h)*

No es parte del pipeline pero lo bloquea: hoy una máquina NVIDIA correría Vulkan
en vez de CUDA, y eso condiciona todas las mediciones posteriores.

1. Completar la matriz driver × gestor (18 combinaciones, hoy 8).
2. `GGML_CUDA=ON` en `third_party/CMakeLists.txt` cuando `nvcc` esté presente,
   con precedencia CUDA > Vulkan > CPU. Instalar el toolkit desde el script.
3. `vulkan-tools` para diagnóstico.
4. **Eliminar la duplicación de tiers**: el script emite `.hw-profile`; el C++
   lo lee si existe en vez de recalcularlo. Una sola fuente de verdad.
5. Verificación activa: `vulkaninfo --summary` debe enumerar un dispositivo,
   no basta con que existan los ficheros.

**Puerta**: en una máquina con NVIDIA, `llm-bench` reporta backend CUDA.

### C — Detector de objetos *(~2 días)*

| Archivo | Contenido |
|---|---|
| `detector/detector-backend.hxx` | `IDetectBackend`, `struct Detection{classId,label,confidence,bbox}` |
| `detector/ort-yolo-backend.*` | ORT-CPU, salida one-to-one `(1,300,6)`, sin NMS. **Primero este**: es el oráculo contra el que se valida el otro |
| `detector/ncnn-yolo-backend.*` | ncnn+Vulkan, one-to-many `(1,84,8400)` + NMS, `vkcache` persistido |
| `detector/detector-service.*` | Selección por tier, sesión única + mutex, métricas |
| `labs/detect-probe/` | `--backend --model --imgsz --image`; imprime ms por etapa y detecciones |
| `scripts/setup.sh` | Descarga del modelo + conversión a ncnn |

**Puerta**: ambos backends detectan las mismas cajas (IoU > 0.9) sobre 10
imágenes; tabla de ms por backend y por `imgsz` 512 vs 640 → **cierra la
decisión con datos**.

### D — Ruta de vídeo de 3 pasadas *(~3 días)*

| Archivo | Contenido |
|---|---|
| `stream/go2rtc-manager.*` | fork/exec RAII, `go2rtc.yaml` generado (validación anti-inyección §19.2), healthcheck, backoff exponencial con techo, `is_online=false` al agotarse |
| `stream/video-decoder.*` | libavcodec + VAAPI/NVDEC según `HardwareProfile`; **salida NV12**; reconexión; métricas `argus_decode_ms` |
| `camera-frame/frame-processor.*` | Motion gate sobre el **plano Y con stride** (0 copias); máscara de `zone`; `warpAffine` único NV12→tensor; mapeo bbox→full-res; crop |
| `camera-frame/crop-quality.*` | Puntuación (tamaño, laplaciano, pose) + trinquete de 3 mejores por ángulo |

**Puerta**: `argus_frame_to_tensor_ms` medido; el gate descarta >90% de frames en
escena estática; comparativa Opción A (libavcodec in-process) vs B (ffmpeg hijo)
→ se elige con el dato, no por preferencia.

### E — Tracking e identidad *(~2 días)*

| Archivo | Contenido |
|---|---|
| `tracker/byte-tracker.*` | ByteTrack: asociación en 2 pasadas, `cv::KalmanFilter` 8-dim, estados Tentative/Confirmed/Lost, `struct Track` |
| `detection-pipeline/detection-pipeline.*` | Orquesta decoder→gate→detector→tracker→reglas; cola de 1 frame con reemplazo; degradación reactiva |
| `face/face-service.*` (modificar) | `extractMat`/`identifyMat`/`identifyMatAsync` — borra un encode y un decode |
| `person-tracker/person-tracker.*` | Personas activas, dedupe, handoff entre cámaras |

**Puerta**: un solo `trackId` durante un recorrido completo; dos personas
seguidas → dos eventos; reconocimiento facial correcto **a ≥4 m y de perfil**,
contra la línea base a 640 px que debe fallar.

### F — Memoria y escalera de decisión *(~3 días)*

| Archivo | Contenido |
|---|---|
| `database/schema.sql` | `person_tag` (1 tabla) + índice |
| `enums.hxx`, `repositories/person-tag/` | `TagSource` + repositorio (reglas 1 y 3) |
| `memory/tag-vocabulary.hxx` | Vocabulario cerrado — fuente única para enum, gramática y reglas |
| `memory/memory-service.*` | Enrolamiento multi-ángulo, tags, timeline, compactación de perfil |
| `memory/scene-context.*` | Ensamblado L0-L3 con presupuesto de tokens |
| `rules/rule-engine.*` | Puertas 0-1 + `argus_decision_gate_total` |

**Puerta**: **>95% de eventos resueltos sin invocar ningún modelo** durante una
jornada real.

### G — Tools y sesiones de LLM *(~3 días)*

| Archivo | Contenido |
|---|---|
| `llm/prefix-cache.*` | Snapshot/restore con `llama_state_seq_get_data`/`set_data` (§6.3); comprobar `llama_memory_can_shift()` |
| `llm/llm-session.*` | Sesión por cámara sobre el contexto compartido |
| `memory/tool-registry.*` | `dispatch()` + `buildGrammar()` con IDs vivos + fire-and-forget |
| `memory/tool-parser.*` | Parser del DSL (trivial: la gramática garantiza la forma) |

**Puerta**: TTFT < 150 ms en el 2º turno; **0 fallos de parseo en 200 tool
calls**; ahorro de tokens vs JSON medido, no estimado.

### H — Cámara al cliente y conversación *(~3 días)*

> ⚠️ **Corrección de diseño (decisiva).** Las revisiones anteriores proponían
> **WHEP con media directa** desde go2rtc al móvil. Eso **solo funciona dentro
> de la casa**: la conexión WebRTC es peer-to-peer contra un puerto que un túnel
> no expone, así que fuera de casa haría falta TURN — otro servicio, otro puerto
> y otro punto de fallo, justo lo contrario de centralizar.
>
> **Decisión: todo el tráfico va por el puerto del backend (7024/HTTPS).** Es el
> único diseño que se comporta igual dentro y fuera de casa, y el único
> compatible con el túnel que va a intermediar con la nube.

**Relay de media en passthrough**

```
GET /camera/:id/stream.mp4      [JwtFilter → RoleFilter(Read)]
   backend ──► 127.0.0.1:1984/api/stream.mp4?src=camN   (fMP4, H.264)
   backend ──chunked, byte a byte──► cliente (LAN o túnel, da igual)
```

El backend **no decodifica ni recodifica**: copia bytes ya codificados de un
socket local a la respuesta HTTP. El overhead es un `memcpy` por fragmento, no
un códec. La cámara sigue viendo **una** sesión RTSP aunque haya N espectadores,
porque go2rtc reparte el mismo ingest.

**Coste honesto del cambio**: fMP4 progresivo tiene más latencia que WebRTC
directo (~0,5-1,5 s frente a 0,2-0,5 s) y el backend pasa de ~0% a copiar
2-4 Mbps por espectador. A cambio: un solo puerto, una sola ruta de código, y
funciona igual desde el sofá que desde otro país. Para vigilancia + asistente,
sub-segundo es suficiente; la alternativa era una ruta que falla justo cuando
más se necesita.

| Archivo | Contenido |
|---|---|
| `stream/go2rtc-manager.*` | fork/exec RAII, `go2rtc.yaml` generado con validación anti-inyección (§19.2), healthcheck, backoff exponencial con techo, `is_online=false` al agotarse |
| `stream/media-relay.*` | Relay HTTP passthrough go2rtc → cliente, chunked, con cancelación al desconectar y límite de espectadores por cámara |
| `api/camera/` | Controllers (4-8 líneas, regla 12) + `CameraService` + DTOs autovalidantes; registro con prueba de conexión real que **no persiste si falla** |
| `socket/sync` | `camera_control` (Update + semáforo por cámara), `voice_start`/`voice_stop` + audio binario PCM 16 kHz, `ai_event` con `trackId`, `voice_state` |
| `conversation/*` | `SentenceAccumulator` (90 chars la 1ª frase, 250 el resto), `ConversationPipeline` con `tts_lookahead=1`, cancelación en LLM/TTS/VLM/STT **y en el bucle de envío del 8800**, gate anti-eco |

**Sin cambios en RBAC**: al ser `GET /camera/:id/stream.mp4`, el mapa actual ya
lo resuelve como **Read**, así que Guard y Guest ven vídeo sin tocar
`role-access.hxx`. La excepción que hacía falta para `POST .../whep` desaparece
con el rediseño.

**Puertas**: live 2K en la app con <1,5 s de latencia y sin transcodificación
(CPU del backend ≈ copia de bytes); el mismo endpoint funciona a través de un
proxy HTTP genérico; el altavoz calla en **<100 ms** al interrumpir.

### Orden y paralelismo

```
C0 ──► C ──► D ──► E ──► F ──► G
                    │            
                    └──► H (WHEP puede ir en paralelo desde D)
```

**Regla de trabajo**: ninguna fase se da por cerrada sin su puerta medida. Las
cifras de este plan que no vengan de una medición van marcadas como estimación.

---

## 10. Riesgos

| Riesgo | Severidad | Mitigación |
|---|---|---|
| ~~El disparador `»` no es un token único~~ | ✅ **resuelto** | LFM2.5 trae `<|tool_call_start|>` nativo en el vocabulario (§4.3a). Un solo token, entrenado para esto |
| `llama_memory_seq_rm`/`seq_cp` no son fiables con las 10 capas conv recurrentes de LFM2.5 | 🟠 **Alta** | Mecanismo cambiado a snapshot + restore con `llama_state_seq_get_data`/`set_data` (§6.3). Verificar `llama_memory_can_shift()` en el arranque y avisar por log |
| Un modelo pequeño (350M) no tiene capacidad para usar tools con criterio | 🟠 Media | La gramática garantiza la forma, no el juicio. En tier Minimal, `tools_enabled = false` |
| El refactor de `LlmService` rompe a los llamadores actuales | 🟡 **Baja** | Auditado: **hay un solo llamador** (`labs/voice-test/voice-test.cc:296`). Es también el test de regresión |
| Reexportar el VLM para recortar los logits de prefill sale mal | 🟡 Baja | Es una optimización opcional (§6.6 #10); el modelo actual sigue funcionando sin ella |
| ncnn/Vulkan más lento que CPU en alguna iGPU | 🟡 Media | `detect-probe --backend` mide ambos por máquina; `force_detector_backend` permite fijarlo |
| Vocabulario de tags que crece sin control | 🟡 Media | Cerrado y versionado en un solo archivo; añadir un tag es editar una línea y regenerar la gramática |
| El modelo etiqueta mal a una persona | 🟡 Media | `person_tag.source` distingue `llm` de `user`; los tags de usuario ganan siempre y son revisables desde la app |
| Degradación reactiva oscilando (histéresis mala) | 🟡 Baja | Ventanas asimétricas: bajar tras 5 s, subir tras 30 s |
| `person_tag` es una migración de esquema | 🟡 Baja | Una tabla, sin tocar existentes; `schema_version` ya está en el esquema |
| Perfiles que crecen y encarecen el prompt | 🟡 Baja | Compactación con `profile_max_chars`; `argus_person_profile_tokens` lo vigila |

---

## 11. Puntos abiertos

1. ✅ **Cerrado** — el disparador de tools es `<|tool_call_start|>`, token nativo de LFM2.5 (§4.3a).
2. **`llama-cpp` con Vulkan/CUDA**: las opciones existen en el recipe, pero **hay que verificar que
   el paquete compila y enlaza** con `with_vulkan=True` en esta máquina antes de meterlo en
   `setup.sh`. Hoy el paquete instalado es solo CPU (`libggml-cpu.a`, sin `libggml-vulkan.a`).
3. **Búsqueda de texto libre sobre eventos (FTS5)** — **fuera de alcance por ahora.** Tags + SQL por
   persona y fecha es exacto, rápido y sin dependencias. Si más adelante quieres *"¿qué pasó el
   martes con el repartidor?"* en lenguaje natural, se revisa entonces; añadirlo ahora sería un paso
   de más para una necesidad que no está confirmada.
4. **Auto-enrolamiento sin intervención del LLM** — ¿alta automática tras N detecciones con crops de
   calidad, o siempre vía `»save`? Con el filtro de `CropQuality` el automático es defendible, pero
   crea personas sin nombre. Propuesta: auto-crear como `person` anónima (`name=''`,
   `#sin-identificar`) y que el `»save` posterior solo le ponga nombre y tags.
5. **Calibración de `min_track_hits`** — 3 hits (~450 ms) vs 2 hits (~300 ms). Se cierra midiendo la
   tasa de falsos positivos real en Fase E, con la escena de la casa.
6. **¿Argus se va a distribuir?** Sigue abierto y sigue condicionando el detector: la AGPL-3.0 de
   YOLO26 cubre código, modelos y pesos, y el disparador es la distribución o el acceso en red de
   terceros. `IDetectBackend` mantiene la decisión reversible (alternativa: RF-DETR-Nano, Apache-2.0).
7. **Retención de crops faciales** — los embeddings no son reversibles a imagen, pero ¿se guarda
   algún thumbnail para que el usuario pueda revisar y corregir identidades desde la app? Es una
   decisión de privacidad, no técnica.

---

## 12. Fuentes verificadas

### API de `llama.cpp` (verificado en el header del pin instalado)

`~/.conan2/p/b/llamacb0fb78ee1dbf/p/include/llama.h` — `llama-cpp/b6565`:

| Línea | Símbolo | Uso en este plan |
|---|---|---|
| 1215 | `llama_sampler_init_grammar(vocab, grammar_str, grammar_root)` | Gramática GBNF estricta |
| 1234 | **`llama_sampler_init_grammar_lazy_patterns(...)`** | **Gramática perezosa por patrón/token: habla libre + tool calls forzados (§4.3a)** |
| 475 | `llama_get_memory(ctx)` | Acceso al KV cache |
| 646 | **`llama_memory_seq_cp(mem, src, dst, p0, p1)`** | **Clonar el prefijo L0+L1 prefilado (§4.5)** |
| 637 | `llama_memory_seq_rm` | Recortar L2/L3 entre turnos |
| 654 | `llama_memory_seq_keep` | Conservar una secuencia |
| 758 | **`llama_state_seq_get_size` / `get_data` / `set_data`** | **Persistir la sesión a disco (§4.5)** |
| 368 | `llama_logit_bias` | Sesgo de tokens, alternativa ligera a la gramática |
| 528 | **`llama_model_chat_template(model, nullptr)`** | **Leer la plantilla del GGUF en vez de hardcodear ChatML (§6.4)** |
| 1065 | `llama_chat_apply_template` | Aplicarla. **Aviso del propio header: no usa parser jinja, solo una lista predefinida** → hace falta fallback |
| 1211-1218 | `llama_sampler_init_penalties(penalty_last_n, penalty_repeat, penalty_freq, penalty_present)` | El header documenta `penalty_freq // 0.0 = disabled`. El código pasa **1.2** (§6.1 #1) |
| 694 | `llama_memory_can_shift(mem)` | Consultar si el modelo soporta shifting — **falso o restringido en arquitecturas recurrentes** como LFM2.5 (§6.3) |
| 1086-1092 | Ejemplo de cadena de samplers en el comentario | Documenta el orden canónico `top_k → top_p → temp → dist` (§6.1 #2) |

### Metadatos del modelo (verificado leyendo el GGUF)

`models/llm/LFM2.5-1.2B-Instruct-Q4_K_M.gguf`:

| Hallazgo | Consecuencia |
|---|---|
| **Arquitectura híbrida: 6 bloques con atención, 10 solo `shortconv`** | Solo 6 capas tienen KV cache → cuantizarlo es un mal trade (§6.1 #3); el estado conv no se puede truncar por posición (§6.3) |
| Plantilla de chat = **ChatML** (`<\|im_start\|>` / `<\|im_end\|>`) | El `buildPrompt` hardcodeado **acierta hoy**; el riesgo es cambiar de modelo (§6.4) |
| Tokens nativos de tool-calling: `<\|tool_call_start\|>`, `<\|tool_call_end\|>`, `<\|tool_list_start\|>`, `<\|tool_list_end\|>`, `<\|tool_response_start\|>`, `<\|tool_response_end\|>` | Disparador de tools dedicado y entrenado; cierra el punto abierto nº1 (§4.3a) |

### Opciones de Conan (verificado en el recipe)

`~/.conan2/p/llama200edf6c3fac3/e/conanfile.py`:
`with_cuda`, `with_vulkan`, `with_curl`, `with_examples`, `shared` — todas `False` por defecto.
`with_vulkan=True` añade `vulkan-loader/[>=1.3 <1.5]` y define `GGML_VULKAN`.
**Estado actual del paquete instalado: solo CPU** (`libggml-cpu.a`, sin `libggml-vulkan.a`).

### Código local

| Ruta | Qué se verificó |
|---|---|
| `src/shared/services/tapo/` | 2991 líneas; Fase 1 completa (transportes, muxer TS, canal talk) |
| `third_party/CMakeLists.txt:48-67` | `NCNN_VULKAN=ON`, `NCNN_AVX2=ON`, `NCNN_OPENMP=ON` → sonda Vulkan sin dependencias nuevas |
| `src/shared/wrapper/thread-budget/thread-budget.cc` | Solo cuenta hilos; no sabe de aceleradores |
| `src/shared/services/llm/llm-service.hxx` | Clase estática, un `context_`, `resetContext{true}`, `std::mutex` global |
| `database/schema.sql:28-135` | `person.observation`, `face_embedding.angle_label`/`quality`, `person_event.confidence`, `context_note.tags`/`valid_from`/`valid_until` |
| `AGENTS.md` reglas 1, 2, 3, 13, 13b, 13c, 17 | Enums para columnas con CHECK · structs de parámetros · estructura de repositorios · arquitectura de `FaceService` · **prohibición de hardcodear hilos** · coroutines para IA · `-march=native` en Release |
| `config.toml` | `number_of_threads = 0`, `client_max_websocket_message_size = "128K"` |
| `CMakeLists.txt:24-25` | `add_subdirectory(voice-test)` / `add_subdirectory(tapo-probe)` → a actualizar |

### Externas

- Ultralytics YOLO26 (arquitectura dual-head, `end2end`, benchmarks, AGPL-3.0/Enterprise):
  https://docs.ultralytics.com/models/yolo26/
- Export a NCNN: https://docs.ultralytics.com/integrations/ncnn
- FAQ ncnn Vulkan (fallback de capas, caveat de GPUs débiles):
  https://github.com/Tencent/ncnn/wiki/FAQ-ncnn-vulkan
- RF-DETR (Apache-2.0, alternativa sin AGPL): https://rfdetr.roboflow.com/latest/
- OpenCV — Video IO hardware acceleration (VAAPI, estado *preview*):
  https://github.com/opencv/opencv/wiki/Video-IO-hardware-acceleration
- Frigate — Face recognition (resolución necesaria para precisión facial):
  https://docs.frigate.video/configuration/face_recognition/
- go2rtc — Tapo (two-way audio nativo) y WebRTC/WHEP:
  https://github.com/AlexxIT/go2rtc/blob/master/internal/tapo/README.md ·
  https://github.com/AlexxIT/go2rtc/blob/master/internal/webrtc/README.md
- pytapo — transporte real de cámaras (`stok` + `securePassthrough`), ya implementado en Argus:
  https://github.com/JurajNyiri/pytapo/blob/main/pytapo/transport/pytapo/pytapo.py

---

*Plan creado el 2026-08-07. Sustituye a `CAMERA_INTEGRATION_PLAN.md` y
`CAMERA_INTEGRATION_REVIEW.md`.*
