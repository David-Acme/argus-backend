# Plan de estabilidad y tiempo real — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dejar el vídeo por `/sync` y la conversación por la cámara funcionando de forma continua y predecible: sin fugas de datos en el parser fMP4, sin upstreams zombis, sin audio troceado, con un VAD que no corta frases ni alucina, y con las piezas probadas subidas de `labs/` a `src/` siguiendo la arquitectura del proyecto.

**Architecture:** Tres capas que hoy están mezcladas se separan. (1) **Transporte**: `upstream-http` deja de adivinar el encoding y entrega *fragmentos* fMP4 completos con keyframe real; `StreamHub` gestiona el ciclo de vida del upstream y mueve los créditos de flujo al sink (una ventana por conexión TCP, que es donde está el cuello real). (2) **Audio DSP**: nace `src/shared/wrapper/audio/` con un resampler **con estado** y un ring buffer, y todo lo que hoy resamplea por bloques pasa por ahí. (3) **VAD**: se corrige el contexto de Silero, se elimina el coste por ventana, se añade una puerta de calidad de turno y sube a `src/shared/services/vad/` como clase de instancia (una por stream, a diferencia de los servicios de IA que son estáticos).

**Tech Stack:** C++20, Drogon (HTTP/WS + logging), ONNX Runtime (Silero VAD), llama.cpp + libmtmd, sherpa-onnx, OpenCV headless, FFmpeg (solo en `labs/`), go2rtc como broker de medios, Conan 2 + CMake presets `dev`/`prod`, Ninja.

## Global Constraints

Estas reglas de `AGENTS.md` y `CONTEXT.md` aplican a **todas** las tareas:

- **Cero comentarios en el código.** El *porqué* va a `CONTEXT.md`, nunca al `.cc`/`.hxx`.
- Cabeceras `.hxx`, fuentes `.cc`. Nunca `.h` ni `.cpp`.
- **Structs de parámetros para 3 o más argumentos**, en todas las capas. Se construyen con designated initializers de C++20 (`{.campo = valor, ...}`), en orden de declaración y **listando todos los miembros** (evita `-Wmissing-field-initializers`). El struct vive en el `*-query.hxx` correspondiente o, si no lo hay, en la cabecera de la clase que declara la función.
- **Inyección de dependencias manual**: miembros privados con sufijo `_`. Nunca métodos estáticos para clases de servicio nuevas, nunca repositorios/servicios como variables locales o temporales.
- **Solo smart pointers.** Ningún puntero propietario crudo; `std::unique_ptr` con deleter propio para recursos de servicio. Punteros crudos solo para acceso no propietario (`.get()`).
- **Nunca hardcodear número de hilos.** Todo sale de `src/shared/wrapper/thread-budget/`.
- **NO spdlog** (se usa `LOG_INFO`/`LOG_WARN`/`LOG_FATAL` de Drogon), **NO `std::future`** (usar `std::thread` + `join`), **NO ORM**.
- Los tunables nuevos van a `config.toml` con su sección y comentario, nunca como constante en el código.
- Códigos de error a través de `AppConfig::ERROR_CODE_*`; nunca literales sueltos.
- Antes de cada commit: `cmake --build --preset dev -j 8` con **0 errores y 0 avisos** en código de Argus, y `clang-format` limpio en los ficheros tocados.
- Conventional Commits **en inglés**.
- **NO tocar `SttService` ni `LlmService` por dentro.** Su rendimiento ya es el
  deseado (transcripción y generación instantáneas en la máquina de referencia) y
  cualquier cambio ahí es riesgo puro sin beneficio. Se pueden cambiar sus
  *llamantes* (cuándo se les llama, con qué audio, con qué historial), nunca su
  implementación ni su configuración de hilos o de modelo.
- Este proyecto no tiene framework de tests unitarios: **la verificación son las sondas de `labs/`**, binarios con aserciones `check(nombre, obtenido, esperado)`. Toda tarea que añada comportamiento añade su aserción a una sonda, y las sondas nuevas deben poder correr **sin cámara física**.
- Regla del proyecto para medir: esta máquina tiene 13-20% de varianza entre ejecuciones. **Una sola corrida no es evidencia**; toda medición de rendimiento se repite 3 veces y se reporta la mediana.

---

## Estructura de ficheros

| Fichero | Responsabilidad |
|---|---|
| `src/shared/wrapper/audio/audio-resampler.{hxx,cc}` | **Nuevo.** `AudioResampler`: sinc con ventana Blackman y **estado entre llamadas**. Convierte cualquier ritmo a cualquier ritmo sin perder muestras en los bordes de bloque. |
| `src/shared/wrapper/audio/sample-ring.hxx` | **Nuevo.** `SampleRing`: buffer circular de `float` de capacidad fija. Sustituye los `vector::erase(begin, begin+N)` O(n) del camino de audio. |
| `src/shared/services/vad/vad-service.{hxx,cc}` | **Nuevo.** `VadService`: Silero v5, clase de **instancia** (una por stream de audio), sin asignaciones por ventana, con puerta de calidad de turno. |
| `src/shared/services/stream/upstream-http.{hxx,cc}` | Encoding decidido por cabeceras; `Fmp4Reader` entrega **fragmentos** (moof+mdat) con keyframe leído de `trun`/`tfhd`. |
| `src/shared/services/stream/stream-hub.{hxx,cc}` | Ciclo de vida del upstream (muerte y recreación), orden de locks único, créditos delegados al sink, límites por conexión. |
| `src/feature/socket/sync/services/sync-service.{hxx,cc}` | `DrogonStreamSink` con ventana de créditos por conexión y `subId` por suscripción; registro de sinks como miembro, no como global de fichero. |
| `src/shared/services/tapo/tapo-audio.{hxx,cc}` | `resample()` pasa a delegar en `AudioResampler` (compatibilidad para llamadas de una pasada). |
| `src/shared/services/tapo/tapo-talk-client.{hxx,cc}` | AGC sin desbordamiento; sesión reutilizable entre frases. |
| `labs/audio-probe/` | **Nueva sonda.** Verifica resampler, ring y VAD **offline** contra ficheros WAV. Es la puerta de calidad del audio. |
| `labs/stream-probe/stream-probe.cc` | Aserciones offline del `Fmp4Reader` (sin go2rtc ni cámara) además de las actuales. |
| `labs/voice-test/{voice-test,camera-audio,vad}.*` | Consumen las piezas nuevas; se borra el código muerto y el VAD local. |
| `config.toml` | Secciones `[vad]` y ampliación de `[streaming]` y `[tapo]`. |
| `CONTEXT.md`, `AGENTS.md` | Memoria y reglas actualizadas al final. |

---

## Fase 0 — Higiene (antes de cualquier commit)

### Task 0: Sacar las credenciales del repositorio

**Files:**
- Modify: `config.toml:245-251`
- Modify: `.gitignore`
- Create: `config.local.toml.example`
- Modify: `src/shared/services/config-service/config-service.cc` (carga del overlay)
- Modify: `labs/tts-probe/tts-probe.cc:212`

**Interfaces:**
- Consumes: nada.
- Produces: `ConfigService::load()` acepta un overlay local opcional; las claves `voice_test.*` dejan de estar en el fichero versionado.

- [ ] **Step 1: Comprobar que la fuga no está aún publicada**

```bash
git log -p --all -S 'Davidcastro' -- config.toml | head
```

Si no devuelve nada, la contraseña **no** está en la historia y basta con no commitearla. Si devuelve algo, hay que rotar la contraseña de la cámara además de limpiar.

- [ ] **Step 2: Mover las claves al overlay local**

Quitar el bloque `[voice_test]` de `config.toml` y dejar solo la referencia en `config.local.toml.example`:

```toml
[voice_test]
camera_rtsp_main = "rtsp://USER:PASSWORD@192.168.1.10:554/stream1"
camera_rtsp_sub = "rtsp://USER:PASSWORD@192.168.1.10:554/stream2"
camera_name = "cam1"
```

- [ ] **Step 3: Cargar el overlay en `ConfigService`**

En `ConfigService::load()`, tras parsear `config.toml`, fusionar `config.local.toml` si existe (las claves del overlay ganan). Firma con struct porque son 3 datos:

```cpp
struct ConfigLoadInput
{
  std::string path;
  std::string overlayPath;
  bool requireOverlay;
};

bool ConfigService::load(const ConfigLoadInput& input);
```

- [ ] **Step 4: Ignorar artefactos**

Añadir a `.gitignore`:

```
config.local.toml
probe-*.wav
```

Y en `labs/tts-probe/tts-probe.cc:212` escribir los WAV al directorio del binario, no al raíz del repo.

- [ ] **Step 5: Borrar los artefactos sueltos y verificar**

```bash
rm -f probe-*.wav
git status --short | grep -E 'probe-|config.local' && echo FALLO || echo OK
cmake --build --preset dev -j 8
```

- [ ] **Step 6: Commit**

```bash
git add config.toml config.local.toml.example .gitignore src/shared/services/config-service labs/tts-probe
git commit -m "chore(config): move camera credentials to an untracked local overlay"
```

---

## Fase 1 — Bugs que rompen el funcionamiento

### Task 1: `Fmp4Reader` — encoding por cabeceras y fragmentos completos

Hoy el reader adivina si el cuerpo es *chunked* buscando el primer `\n` de cada buffer, y entrega `moof` y `mdat` como cajas sueltas. Dos consecuencias medidas: si un `recv` no contiene ningún `0x0A` sus bytes se descartan en silencio (un `recv` de 200 bytes tiene ~46% de probabilidad), y si el primer `\n` viene precedido de `\r` y un dígito hex el flujo entero desaparece para siempre.

**Files:**
- Modify: `src/shared/services/stream/upstream-http.hxx`
- Modify: `src/shared/services/stream/upstream-http.cc`
- Modify: `src/shared/services/stream/stream-hub.cc:132-143`
- Test: `labs/stream-probe/stream-probe.cc`

**Interfaces:**
- Consumes: `upstream_http::open()` (sin cambios), que ya guarda `Upstream::headers`.
- Produces:
  - `bool upstream_http::isChunked(const std::string& headers);`
  - `struct Fmp4ReaderInput { bool chunked; };`
  - `explicit Fmp4Reader(Fmp4ReaderInput input);`
  - `std::function<void(std::string init)> onInit;`
  - `std::function<void(std::string fragment, bool keyframe)> onFragment;` — **sustituye a `onBox`**. `fragment` es `moof` + `mdat` concatenados.

- [ ] **Step 1: Escribir las aserciones que fallan**

En `labs/stream-probe/stream-probe.cc`, dentro del `namespace` anónimo:

```cpp
std::string mp4Box(const std::string& type, const std::string& payload)
{
  const uint32_t size = static_cast<uint32_t>(8 + payload.size());
  std::string out;
  out += static_cast<char>((size >> 24) & 0xFF);
  out += static_cast<char>((size >> 16) & 0xFF);
  out += static_cast<char>((size >> 8) & 0xFF);
  out += static_cast<char>(size & 0xFF);
  out += type;
  out += payload;
  return out;
}

std::string be32(uint32_t v)
{
  std::string out;
  out += static_cast<char>((v >> 24) & 0xFF);
  out += static_cast<char>((v >> 16) & 0xFF);
  out += static_cast<char>((v >> 8) & 0xFF);
  out += static_cast<char>(v & 0xFF);
  return out;
}

std::string moofWithSync(bool sync)
{
  const uint32_t flags = sync ? 0x00000000U : 0x00010000U;
  const std::string trun = mp4Box("trun", be32(0x00000005) + be32(1) +
                                              be32(0) + be32(flags));
  const std::string tfhd = mp4Box("tfhd", be32(0x00000000) + be32(1));
  const std::string traf = mp4Box("traf", tfhd + trun);
  return mp4Box("moof", mp4Box("mfhd", be32(0) + be32(1)) + traf);
}

std::string chunkEncode(const std::string& body, size_t chunkSize)
{
  std::string out;
  char hex[32];
  for (size_t i = 0; i < body.size(); i += chunkSize) {
    const size_t n = std::min(chunkSize, body.size() - i);
    std::snprintf(hex, sizeof(hex), "%zx\r\n", n);
    out += hex;
    out += body.substr(i, n);
    out += "\r\n";
  }
  out += "0\r\n\r\n";
  return out;
}

struct ReaderCapture
{
  std::string init;
  std::vector<std::string> fragments;
  std::vector<bool> keyframes;
};

ReaderCapture runReader(const std::string& wire, bool chunked, size_t step)
{
  ReaderCapture cap;
  upstream_http::Fmp4Reader reader({.chunked = chunked});
  reader.onInit = [&](std::string box) { cap.init = std::move(box); };
  reader.onFragment = [&](std::string box, bool key) {
    cap.fragments.push_back(std::move(box));
    cap.keyframes.push_back(key);
  };
  for (size_t i = 0; i < wire.size(); i += step)
    reader.feed(wire.data() + i, std::min(step, wire.size() - i));
  return cap;
}

void fmp4ReaderCheck()
{
  std::printf("\n=== fmp4 reader ===\n");

  const std::string init = mp4Box("ftyp", std::string(24, '\x0a')) +
                           mp4Box("moov", std::string(600, '\r'));
  const std::string frag1 = moofWithSync(true) + mp4Box("mdat", std::string(20000, '\n'));
  const std::string frag2 = moofWithSync(false) + mp4Box("mdat", std::string(15000, '\x0d'));
  const std::string body = init + frag1 + frag2;

  check("identity: init completo", runReader(body, false, 65536).init == init, true);
  check("identity: 2 fragmentos", runReader(body, false, 65536).fragments.size() == 2, true);
  check("identity: fragmento 1 intacto",
        runReader(body, false, 65536).fragments[0] == frag1, true);
  check("identity: keyframe detectado", runReader(body, false, 65536).keyframes[0], true);
  check("identity: no-keyframe detectado", runReader(body, false, 65536).keyframes[1], false);

  for (const size_t step : {size_t(1), size_t(3), size_t(199), size_t(4096)}) {
    const auto cap = runReader(body, false, step);
    check("identity: estable troceando el wire", cap.init == init && cap.fragments.size() == 2 &&
                                                     cap.fragments[0] == frag1 &&
                                                     cap.fragments[1] == frag2, true);
  }

  const std::string wire = chunkEncode(body, 1300);
  for (const size_t step : {size_t(1), size_t(2), size_t(1299), size_t(65536)}) {
    const auto cap = runReader(wire, true, step);
    check("chunked: estable troceando el wire", cap.init == init && cap.fragments.size() == 2 &&
                                                    cap.fragments[0] == frag1 &&
                                                    cap.fragments[1] == frag2, true);
  }

  check("isChunked lee la cabecera",
        upstream_http::isChunked("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"), true);
  check("isChunked ignora identity",
        upstream_http::isChunked("HTTP/1.1 200 OK\r\nContent-Type: video/mp4"), false);
}
```

Llamar a `fmp4ReaderCheck();` al principio de `main()`, antes de arrancar go2rtc — no necesita ni proceso ni cámara.

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
cmake --build --preset dev -j 8 --target argus-stream-probe
./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: no compila (`onFragment` y `Fmp4ReaderInput` no existen). Ese es el fallo válido.

- [ ] **Step 3: Reescribir la cabecera**

`src/shared/services/stream/upstream-http.hxx`:

```cpp
bool isChunked(const std::string& headers);

struct Fmp4ReaderInput
{
  bool chunked;
};

class Fmp4Reader
{
public:
  std::function<void(std::string init)> onInit;
  std::function<void(std::string fragment, bool keyframe)> onFragment;

  explicit Fmp4Reader(Fmp4ReaderInput input);

  void feed(const char* data, size_t len);
  void reset();
  bool initDone() const { return initDone_; }

private:
  void consume();
  void processChunked(const char* data, size_t len);
  void emit(std::string box, const std::string& type);

  std::string pending_;
  std::string init_;
  std::string fragment_;
  bool initDone_{false};
  bool hasMoof_{false};
  bool fragmentKeyframe_{false};

  bool chunked_{false};
  std::string lineBuf_;
  size_t chunkRemaining_{0};
  bool inChunkData_{false};
};
```

- [ ] **Step 4: Implementar el troceado chunked correcto**

En `upstream-http.cc`, sustituir `feed`/`processChunked` por una máquina de estados que **no adivina** y que no pierde bytes cuando el CRLF cae entre dos buffers:

```cpp
bool isChunked(const std::string& headers)
{
  std::string lower = headers;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("transfer-encoding: chunked") != std::string::npos;
}

Fmp4Reader::Fmp4Reader(Fmp4ReaderInput input) : chunked_(input.chunked) {}

void Fmp4Reader::feed(const char* data, size_t len)
{
  if (len == 0)
    return;
  if (!chunked_) {
    pending_.append(data, len);
    consume();
    return;
  }
  processChunked(data, len);
}

void Fmp4Reader::processChunked(const char* data, size_t len)
{
  size_t pos = 0;
  while (pos < len) {
    if (inChunkData_) {
      const size_t take = std::min(chunkRemaining_, len - pos);
      pending_.append(data + pos, take);
      chunkRemaining_ -= take;
      pos += take;
      if (chunkRemaining_ == 0) {
        inChunkData_ = false;
        lineBuf_.clear();
      }
      consume();
      continue;
    }

    lineBuf_ += data[pos];
    ++pos;
    if (lineBuf_.size() > 64) {
      lineBuf_.clear();
      continue;
    }
    if (lineBuf_.size() < 2 || lineBuf_[lineBuf_.size() - 1] != '\n' ||
        lineBuf_[lineBuf_.size() - 2] != '\r')
      continue;

    const std::string line = lineBuf_.substr(0, lineBuf_.size() - 2);
    lineBuf_.clear();
    if (line.empty())
      continue;
    bool ok = false;
    const size_t size = parseChunkSize(line, ok);
    if (!ok || size == 0)
      continue;
    chunkRemaining_ = size;
    inChunkData_ = true;
  }
}
```

La diferencia estructural: el terminador de chunk se consume **byte a byte acumulando en `lineBuf_`**, así que da igual dónde parta el `recv`. El `continue` sobre línea vacía absorbe el CRLF que cierra cada chunk sin contarlo como tamaño.

- [ ] **Step 5: Emitir fragmentos y leer el keyframe real**

```cpp
namespace
{

bool syncFromSampleFlags(uint32_t flags)
{
  return (flags & 0x00010000U) == 0;
}

bool moofIsKeyframe(const std::string& moof)
{
  bool found = false;
  bool sync = true;
  const auto* raw = reinterpret_cast<const uint8_t*>(moof.data());
  const size_t total = moof.size();

  std::function<void(size_t, size_t)> walk = [&](size_t begin, size_t end) {
    size_t offset = begin;
    while (offset + 8 <= end) {
      const uint64_t size = (static_cast<uint64_t>(raw[offset]) << 24) |
                            (static_cast<uint64_t>(raw[offset + 1]) << 16) |
                            (static_cast<uint64_t>(raw[offset + 2]) << 8) |
                            static_cast<uint64_t>(raw[offset + 3]);
      if (size < 8 || offset + size > end)
        return;
      const std::string type(moof, offset + 4, 4);
      const size_t body = offset + 8;
      if (type == "traf")
        walk(body, offset + size);
      else if (type == "tfhd" && body + 8 <= end) {
        const uint32_t tf = readBe32(raw + body) & 0x00FFFFFFU;
        size_t cursor = body + 8;
        if (tf & 0x000001U) cursor += 8;
        if (tf & 0x000002U) cursor += 4;
        if (tf & 0x000008U) cursor += 4;
        if (tf & 0x000010U) cursor += 4;
        if ((tf & 0x000020U) && cursor + 4 <= end) {
          sync = syncFromSampleFlags(readBe32(raw + cursor));
          found = true;
        }
      }
      else if (type == "trun" && body + 8 <= end) {
        const uint32_t tr = readBe32(raw + body) & 0x00FFFFFFU;
        size_t cursor = body + 8;
        if (tr & 0x000001U) cursor += 4;
        if ((tr & 0x000004U) && cursor + 4 <= end) {
          sync = syncFromSampleFlags(readBe32(raw + cursor));
          found = true;
        }
      }
      offset += size;
    }
  };

  walk(0, total);
  return found ? sync : true;
}

} // namespace

void Fmp4Reader::emit(std::string box, const std::string& type)
{
  if (!initDone_) {
    init_ += box;
    if (type == "moov") {
      initDone_ = true;
      if (onInit)
        onInit(std::move(init_));
      init_.clear();
    }
    return;
  }

  if (type == "moof") {
    fragment_ = std::move(box);
    fragmentKeyframe_ = moofIsKeyframe(fragment_);
    hasMoof_ = true;
    return;
  }

  if (type == "mdat" && hasMoof_) {
    fragment_ += box;
    hasMoof_ = false;
    if (onFragment)
      onFragment(std::move(fragment_), fragmentKeyframe_);
    fragment_.clear();
  }
}
```

`consume()` mantiene el bucle actual de troceado de cajas y llama a `emit(box, type)` en vez de decidir ahí. Añadir el helper `readBe32` junto a `boxSize`.

- [ ] **Step 6: Adaptar `StreamHub`**

En `stream-hub.cc:132-143`, construir el reader con el encoding real y renombrar el callback:

```cpp
  upstream_http::Fmp4Reader reader({.chunked = upstream_http::isChunked(conn.headers)});
  reader.onInit = [&up](std::string box) {
    std::lock_guard<std::mutex> lock(up->mtx);
    up->init = std::move(box);
    up->hasInit = true;
  };
  reader.onFragment = [&up](std::string box, bool keyframe) {
    dispatchBox(*up, std::move(box), keyframe);
  };
```

- [ ] **Step 7: Verificar**

```bash
cmake --build --preset dev -j 8 --target argus-stream-probe
./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: las 15 aserciones de `=== fmp4 reader ===` en OK, incluidas las 8 de troceado adversario (`step=1`).

- [ ] **Step 8: Commit**

```bash
git add src/shared/services/stream/upstream-http.hxx src/shared/services/stream/upstream-http.cc \
        src/shared/services/stream/stream-hub.cc labs/stream-probe/stream-probe.cc
git commit -m "fix(stream): read transfer encoding from headers and emit whole fmp4 fragments"
```

---

### Task 2: Ciclo de vida del upstream y orden de locks

Cuando el hilo lector termina (fin de la ventana de gracia, EOF o fallo) su entrada sigue en `upstreams_`, así que la siguiente suscripción recibe un `subId` válido sobre un upstream muerto y **no llega un byte hasta reiniciar el backend**. Además `subscribe()` toma `up->mtx` y dentro `hubMutex_`, mientras `activeSubscribers()` los toma al revés: ABBA.

**Files:**
- Modify: `src/shared/services/stream/stream-hub.hxx:54-64`
- Modify: `src/shared/services/stream/stream-hub.cc:109-268`
- Test: `labs/stream-probe/stream-probe.cc`

**Interfaces:**
- Consumes: `upstream_http::open`, `Fmp4Reader` de la Task 1.
- Produces: invariante de orden de locks **`hubMutex_` → `Upstream::mtx`, nunca al revés**; `StreamHub::activeUpstreams()` deja de contar upstreams muertos.

- [ ] **Step 1: Escribir la aserción que falla**

Añadir a `streamHubCheck()` en `labs/stream-probe/stream-probe.cc`, después de la comprobación de `unsubscribe`:

```cpp
  StreamHub::unsubscribe(subId);
  std::this_thread::sleep_for(std::chrono::milliseconds(3500));
  check("hub cierra el upstream sin suscriptores", StreamHub::activeUpstreams() == 0, true);

  auto sink2 = std::make_shared<FakeSink>();
  StreamHub::SubscribeInput again;
  again.sink = sink2;
  again.cameraId = 1;
  again.quality = "main";
  std::string err2;
  const uint16_t subId2 = StreamHub::subscribe(again, err2);
  check("hub resuscribe tras la gracia", subId2 != 0, true);
  for (int i = 0; i < 60 && !sink2->mediaSeen; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  check("hub vuelve a entregar media tras la gracia", sink2->mediaSeen, true);
  StreamHub::unsubscribe(subId2);
```

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: `hub vuelve a entregar media tras la gracia` falla (`0 != 1`) porque el upstream zombi no relee nada.

- [ ] **Step 3: Marcar el upstream muerto**

En `stream-hub.hxx`, añadir a `struct Upstream`:

```cpp
    std::atomic<bool> dead{false};
```

Al final de `runUpstream()`, después de cerrar el fd y notificar a los suscriptores y **fuera** de cualquier lock:

```cpp
  up->dead.store(true, std::memory_order_release);
```

- [ ] **Step 4: Recolectar el muerto en `getOrOpen`**

```cpp
  const std::string name = upstreamName(input.cameraId, input.quality);
  std::lock_guard<std::mutex> lock(hubMutex_);
  auto it = upstreams_.find(name);
  if (it != upstreams_.end()) {
    if (!it->second->dead.load(std::memory_order_acquire))
      return it->second;
    if (it->second->reader.joinable())
      it->second->reader.join();
    upstreams_.erase(it);
  }

  auto up = std::make_shared<Upstream>();
  up->name = name;
  up->reader = std::thread(&StreamHub::runUpstream, up);
  upstreams_.emplace(name, up);
  return up;
```

No se hace `detach()`: el hilo sigue siendo joinable y lo une el siguiente `getOrOpen` o el `shutdown()`, que ya recorre el mapa. Así se respeta la regla de `std::thread` + `join`.

- [ ] **Step 5: Distinguir timeout de error real**

En el bucle de `runUpstream`, sustituir el bloque de error por:

```cpp
    if (n == 0)
      break;
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      LOG_WARN << "StreamHub: recv failed on " << up->name << " errno=" << errno;
      break;
    }
```

Sin esto, un error persistente del socket con suscriptores presentes deja el hilo girando a 100% de CPU indefinidamente, porque el código lo trata como si fuera el `SO_RCVTIMEO` de 1 s.

- [ ] **Step 6: Un único orden de locks**

En `subscribe()`, registrar en `subToUpstream_` **antes** de tomar `up->mtx`, y no volver a tocar `hubMutex_` después:

```cpp
  uint16_t subId = 0;
  auto sub = std::make_shared<Subscriber>();
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    for (int attempt = 0; attempt < 65535; ++attempt) {
      subId = nextSubId_++;
      if (nextSubId_ == 0)
        nextSubId_ = 1;
      if (subId != 0 && subToUpstream_.find(subId) == subToUpstream_.end())
        break;
      subId = 0;
    }
    if (subId == 0) {
      error = "no_free_subscription_id";
      return 0;
    }
    subToUpstream_[subId] = up;
  }

  sub->subId = subId;
  sub->sink = input.sink;
  {
    std::lock_guard<std::mutex> upLock(up->mtx);
    up->subs.push_back(sub);
  }
  return subId;
```

Un `ack` que llegue entre el registro y el `push_back` no encuentra el suscriptor y no hace nada, que es el comportamiento correcto.

- [ ] **Step 7: Verificar**

```bash
cmake --build --preset dev -j 8 --target argus-stream-probe
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: `hub vuelve a entregar media tras la gracia` en OK y `activeUpstreams()==0` tras la gracia.

- [ ] **Step 8: Commit**

```bash
git add src/shared/services/stream/stream-hub.hxx src/shared/services/stream/stream-hub.cc \
        labs/stream-probe/stream-probe.cc
git commit -m "fix(stream): recycle dead upstreams and unify hub lock order"
```

---

### Task 3: Créditos por conexión y límites por cliente

La ventana de 128 KB es hoy **por suscripción**, pero todas las suscripciones de un cliente comparten un único socket TCP. Con la rejilla de 6 cámaras del diseño, el backend permite 768 KB en vuelo sobre una conexión cuyo buffer de trantor no tiene tope. Los créditos existen precisamente para que eso no ocurra.

**Files:**
- Modify: `src/shared/services/stream/stream-hub.hxx:15-52`
- Modify: `src/shared/services/stream/stream-hub.cc:38-107,270-292`
- Modify: `src/feature/socket/sync/services/sync-service.hxx`
- Modify: `src/feature/socket/sync/services/sync-service.cc`
- Modify: `config.toml` sección `[streaming]`
- Test: `labs/stream-probe/stream-probe.cc`

**Interfaces:**
- Consumes: `StreamHub::ISink` de la Task 2.
- Produces:
  - `struct StreamClosedInput { uint16_t subId; std::string reason; };`
  - `ISink::tryReserve(size_t bytes) -> bool` y `ISink::release(int64_t bytes)`.
  - `ISink::onClosed(const StreamClosedInput& input)`.
  - `StreamHub::ack(uint16_t subId, int64_t bytes)` pasa a liberar crédito en el **sink**, no en el suscriptor.

- [ ] **Step 1: Escribir la aserción que falla**

En `FakeSink` de `labs/stream-probe/stream-probe.cc`, implementar la ventana y añadir una segunda suscripción sobre el mismo sink:

```cpp
  bool tryReserve(size_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (inFlight + static_cast<int64_t>(bytes) > window)
      return false;
    inFlight += static_cast<int64_t>(bytes);
    return true;
  }

  void release(int64_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mtx);
    inFlight = std::max<int64_t>(0, inFlight - bytes);
  }

  int64_t window{128 * 1024};
  int64_t inFlight{0};
```

Y la comprobación:

```cpp
  StreamHub::SubscribeInput second;
  second.sink = sink;
  second.cameraId = 1;
  second.quality = "sub";
  std::string err3;
  const uint16_t subB = StreamHub::subscribe(second, err3);
  check("segunda suscripcion en el mismo sink", subB != 0, true);
  const size_t before = sink->bytes();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  const size_t after = sink->bytes();
  check("dos suscripciones comparten una sola ventana",
        after - before < 160 * 1024, true);
```

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: no compila (`tryReserve` no es `override` de nada). Tras declarar el interfaz, la aserción falla porque cada suscripción tiene su propia ventana.

- [ ] **Step 3: Mover los créditos al interfaz del sink**

`stream-hub.hxx`:

```cpp
  struct StreamClosedInput
  {
    uint16_t subId;
    std::string reason;
  };

  class ISink
  {
  public:
    virtual ~ISink() = default;
    virtual bool sendBinary(const uint8_t* data, size_t len) = 0;
    virtual bool tryReserve(size_t bytes) = 0;
    virtual void release(int64_t bytes) = 0;
    virtual void onClosed(const StreamClosedInput& input) = 0;
  };
```

Y en `struct Subscriber` desaparece `bytesInFlight`.

- [ ] **Step 4: Reservar antes de enviar**

`sendBox` pasa a pedir crédito al sink:

```cpp
void StreamHub::sendBox(const std::shared_ptr<Subscriber>& sub,
                        const std::string& box, bool keyframe)
{
  size_t offset = 0;
  bool first = true;
  while (offset < box.size()) {
    const size_t part = std::min(chunkBytes_, box.size() - offset);
    if (!sub->sink->tryReserve(part)) {
      sub->skipUntilKeyframe = true;
      return;
    }
    sendFramed(sub, ws_frame::kTypeMedia, keyframe && first,
               reinterpret_cast<const uint8_t*>(box.data() + offset), part);
    if (!sub->sink)
      return;
    offset += part;
    first = false;
  }
}
```

`ack()` localiza el suscriptor solo para obtener su sink y liberar ahí:

```cpp
void StreamHub::ack(uint16_t subId, int64_t bytes)
{
  if (bytes <= 0)
    return;
  std::shared_ptr<Upstream> up;
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    const auto it = subToUpstream_.find(subId);
    if (it == subToUpstream_.end())
      return;
    up = it->second;
  }

  std::shared_ptr<ISink> sink;
  {
    std::lock_guard<std::mutex> upLock(up->mtx);
    for (const auto& sub : up->subs) {
      if (sub->subId == subId) {
        sink = sub->sink;
        break;
      }
    }
  }
  if (sink)
    sink->release(bytes);
}
```

- [ ] **Step 5: Reservar también el segmento de init**

En `dispatchBox`, sustituir la comprobación manual de ventana por `tryReserve(up.init.size())`, manteniendo el `skipUntilKeyframe = true` cuando no hay crédito. Un init que no cabe entero no se manda a medias: el cliente no puede inicializar el decodificador con un `moov` truncado.

- [ ] **Step 6: Implementar la ventana en `DrogonStreamSink`**

En `sync-service.cc`, el sink gana la ventana y el registro de sus `subId`, y `onClosed` reporta el correcto:

```cpp
class DrogonStreamSink final : public StreamHub::ISink
{
public:
  DrogonStreamSink(drogon::WebSocketConnectionPtr conn, int64_t window)
      : conn_(std::move(conn)), window_(window)
  {
  }

  bool sendBinary(const uint8_t* data, size_t len) override
  {
    if (!conn_ || conn_->disconnected())
      return false;
    conn_->send(reinterpret_cast<const char*>(data), len,
                drogon::WebSocketMessageType::Binary);
    return true;
  }

  bool tryReserve(size_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inFlight_ + static_cast<int64_t>(bytes) > window_)
      return false;
    inFlight_ += static_cast<int64_t>(bytes);
    return true;
  }

  void release(int64_t bytes) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    inFlight_ = std::max<int64_t>(0, inFlight_ - bytes);
  }

  void onClosed(const StreamHub::StreamClosedInput& input) override
  {
    if (!conn_ || conn_->disconnected())
      return;
    Json::Value j;
    j["type"] = "camera:closed";
    j["payload"]["subId"] = input.subId;
    j["payload"]["reason"] = input.reason;
    conn_->sendJson(j);
  }

  int subscriptions() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscriptions_;
  }

  void addSubscription() { std::lock_guard<std::mutex> lock(mutex_); ++subscriptions_; }
  void dropSubscription() { std::lock_guard<std::mutex> lock(mutex_); --subscriptions_; }

private:
  drogon::WebSocketConnectionPtr conn_;
  mutable std::mutex mutex_;
  int64_t window_;
  int64_t inFlight_{0};
  int subscriptions_{0};
};
```

En `runUpstream` y `dispatchBox`, las llamadas a `onClosed` pasan el `subId` del suscriptor:

```cpp
      sub->sink->onClosed({.subId = sub->subId, .reason = "upstream_closed"});
```

- [ ] **Step 7: Limitar suscripciones por conexión**

En `SyncService::handleMessage`, antes de suscribir:

```cpp
    if (sink->subscriptions() >= maxSubsPerClient_)
      throw ResponseException("Too many camera subscriptions", 429,
                              AppConfig::ERROR_CODE_TOO_MANY_REQUESTS);
```

Añadir `AppConfig::ERROR_CODE_TOO_MANY_REQUESTS = "TOO_MANY_REQUESTS"` junto al resto de códigos, y sustituir el literal `"SERVICE_UNAVAILABLE"` que hoy está suelto por `AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE`.

- [ ] **Step 8: Mover el registro de sinks a miembro**

El mapa global `gSinks` de `sync-service.cc` pasa a ser miembro de `SyncService` (`std::unordered_map<const void*, std::shared_ptr<DrogonStreamSink>> sinks_;` + `mutable std::mutex sinksMutex_;`), como manda la regla de inyección de dependencias: el estado de un servicio vive en el servicio.

- [ ] **Step 9: Configuración**

En `config.toml`, sección `[streaming]`:

```toml
# Bytes de vídeo en vuelo permitidos por CONEXIÓN (no por suscripción): el
# cuello es el socket TCP, que comparten todas las cámaras que mira un cliente.
hub_window_bytes = 131072
# Trozo máximo por mensaje binario. Acota cuánto puede monopolizar el socket
# un solo fragmento y deja hueco al audio y al JSON de control.
hub_chunk_bytes = 16384
# Segundos que se mantiene abierto el upstream tras irse el último espectador.
hub_grace_ms = 2000
# Cámaras simultáneas por cliente. La rejilla de vista previa marca el mínimo.
hub_max_subs_per_client = 8
```

- [ ] **Step 10: Verificar**

```bash
cmake --build --preset dev -j 8
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: `dos suscripciones comparten una sola ventana` en OK.

- [ ] **Step 11: Commit**

```bash
git add src/shared/services/stream src/feature/socket/sync src/config config.toml labs/stream-probe
git commit -m "fix(stream): make the credit window per connection and bound subscriptions"
```

---

### Task 4: Dos fallos de memoria y aritmética

**Files:**
- Modify: `src/shared/services/tapo/tapo-talk-client.cc:374-380`
- Modify: `labs/voice-test/camera-audio.cc:93`
- Test: `labs/tapo-probe/tapo-probe.cc`

**Interfaces:**
- Consumes: nada nuevo.
- Produces: `CameraMic::readBlock()` devuelve `false` si el micrófono no está abierto.

- [ ] **Step 1: Añadir el modo offline a `tapo-probe`**

`argus-tapo-probe` hoy solo tiene banderas que hablan con la cámara (`--info`, `--talk`, `--ts-dump`…). Añadir `--offline`, que ejecuta únicamente las comprobaciones que no necesitan hardware y sale con código 1 si alguna falla, para que esta tarea sea verificable sin la C225 delante.

- [ ] **Step 2: Escribir la aserción que falla**

En `labs/tapo-probe/tapo-probe.cc`, añadir una comprobación offline del AGC exponiendo la función. Extraer `equalizeForSpeaker` y la ganancia a una función con nombre en el header:

```cpp
struct TapoSpeakerGainInput
{
  std::vector<int16_t> samples;
  double maxGain;
  double targetPeak;
};

std::vector<int16_t> tapoApplySpeakerGain(const TapoSpeakerGainInput& input);
```

Aserción:

```cpp
  std::vector<int16_t> fullScale(64, -32768);
  const auto gained = tapoApplySpeakerGain({.samples = fullScale,
                                            .maxGain = 3.0,
                                            .targetPeak = 26000.0});
  check("AGC no invierte la fase a fondo de escala", gained[0] < 0, true);
  check("AGC no amplifica lo que ya satura", gained[0] >= -32768 && gained[0] <= -20000, true);
```

- [ ] **Step 3: Ejecutar y ver fallar**

```bash
cmake --build --preset dev -j 8 --target argus-tapo-probe
./build/dev/labs/tapo-probe/argus-tapo-probe --offline
```

Esperado: `AGC no invierte la fase a fondo de escala` falla. Medido hoy: con una muestra a `-32768` el pico se guarda en un `int16_t` y vale `-32768`, así que la ganancia sale `-0.793` y la frase entera se emite invertida y atenuada un 20%.

- [ ] **Step 4: Arreglar el AGC**

```cpp
std::vector<int16_t> tapoApplySpeakerGain(const TapoSpeakerGainInput& input)
{
  int peak = 1;
  for (const auto sample : input.samples)
    peak = std::max(peak, std::abs(static_cast<int>(sample)));
  const double gain = std::min(input.maxGain, input.targetPeak / peak);
  std::vector<int16_t> out;
  out.reserve(input.samples.size());
  for (const auto sample : input.samples)
    out.push_back(static_cast<int16_t>(
        std::clamp(static_cast<double>(sample) * gain, -32768.0, 32767.0)));
  return out;
}
```

- [ ] **Step 5: Proteger `CameraMic::readBlock`**

```cpp
bool CameraMic::readBlock()
{
  if (!impl_ || !impl_->fmt || !impl_->dec)
    return false;
```

`runCameraSttCheck` y `runCameraVadCheck` ignoran el retorno de `open()` y llaman a `readBlock()`; con la cámara apagada eso es hoy un segfault directo.

- [ ] **Step 6: Verificar**

```bash
cmake --build --preset dev -j 8
./build/dev/labs/tapo-probe/argus-tapo-probe --offline
./build/dev/labs/voice-test/argus-voice-test --camera-stt-check
```

Esperado: las dos aserciones del AGC en OK; con la cámara apagada, `--camera-stt-check` imprime el error y sale con código 1 en vez de romper.

- [ ] **Step 7: Commit**

```bash
git add src/shared/services/tapo labs/voice-test labs/tapo-probe
git commit -m "fix(audio): correct speaker AGC overflow and camera mic null guard"
```

---

### Task 5: Borrar el código muerto de `voice-test`

`main()` bifurca en `if (useCamera) { runCameraConversation(...); } else { ... }`, así que dentro del `else` la variable es siempre falsa: `captureTurnFromCamera()`, el `!useCamera && openPlayback(...)` y el `if (useCamera) speakToCamera(...)` del hilo `speaker` **no se ejecutan nunca**. Además `runCameraConversation` duplica el bucle de conversación con comportamiento distinto (sin TTS frase a frase, sin métrica de primer audio).

**Files:**
- Modify: `labs/voice-test/voice-test.cc`

**Interfaces:**
- Consumes: nada nuevo.
- Produces: un único bucle de conversación parametrizado por la fuente de audio.

- [ ] **Step 1: Unificar el bucle**

Extraer el bucle del `else` a una función con struct de entrada y hacer que `runCameraConversation` desaparezca:

```cpp
struct ConversationInput
{
  bool useCamera;
  std::string langCode;
  std::string cameraRtspSub;
  std::string cameraRtspMain;
  TapoTalkConfig talkConfig;
};

void runConversation(const ConversationInput& input);
```

La única diferencia entre los dos caminos queda en dos puntos: de dónde salen las muestras (micrófono local o `CameraMic`) y a dónde va el PCM (PulseAudio o el canal de voz de la cámara). Todo lo demás — troceado por frases, historial, VLM cuando se menciona la cámara, métricas — es común.

- [ ] **Step 2: Borrar lo muerto**

Eliminar `captureTurnFromCamera()` y `runCameraConversation()` completos, y los `useCamera ?` del cuerpo unificado que ya no aplican.

- [ ] **Step 3: Quitar los comentarios del código nuevo**

Los bloques añadidos en esta rama llevan comentarios (`// Samples de audio del mic a descartar...`, `// Argus habla: descartar`, `// eco residual del altavoz: descartar`). La regla del proyecto es cero comentarios: lo que expliquen se traslada a `CONTEXT.md` en la Task 14.

- [ ] **Step 4: Formatear**

```bash
clang-format -i labs/voice-test/voice-test.cc labs/voice-test/camera-audio.cc \
                src/shared/services/tapo/tapo-talk-client.cc \
                src/shared/services/tapo/secure-passthrough-transport.cc \
                src/shared/services/stream/stream-hub.cc \
                src/shared/services/stream/upstream-http.cc \
                src/feature/socket/sync/services/sync-service.cc
```

Estado actual medido: 246 reemplazos pendientes en `voice-test.cc`, 22 en `tapo-talk-client.cc`, 17 en `secure-passthrough-transport.cc`.

- [ ] **Step 5: Verificar**

```bash
cmake --build --preset dev -j 8
for f in labs/voice-test/voice-test.cc src/shared/services/stream/stream-hub.cc; do
  clang-format --output-replacements-xml "$f" | grep -c "<replacement " ; done
```

Esperado: build limpio y `0` reemplazos en ambos.

- [ ] **Step 6: Commit**

```bash
git add labs/voice-test src/shared/services src/feature
git commit -m "refactor(voice-test): unify the conversation loop and drop dead camera paths"
```

---

## Fase 2 — Audio en tiempo real

### Task 6: `AudioResampler` con estado y `SampleRing`

El resampler actual es sinc con ventana Blackman pero **sin estado**: arranca en `pos = kSincHalf` y corta en `pos + kSincHalf < size`. Aplicado por bloques de 1024 muestras a 8→16 kHz eso **descarta el 6,25% del audio** — medido: `in=1024 esperado=2048 obtenido=1920` — con un hueco de ~8 ms cada 128 ms y la línea temporal comprimida un 6%. Ese es el audio que hoy llega al VAD y a Whisper por el camino de la cámara.

**Files:**
- Create: `src/shared/wrapper/audio/audio-resampler.hxx`
- Create: `src/shared/wrapper/audio/audio-resampler.cc`
- Create: `src/shared/wrapper/audio/sample-ring.hxx`
- Create: `labs/audio-probe/CMakeLists.txt`
- Create: `labs/audio-probe/audio-probe.cc`
- Modify: `CMakeLists.txt:28` (añadir `add_subdirectory(labs/audio-probe)`)

**Interfaces:**
- Consumes: nada.
- Produces:
  - `struct AudioResamplerInput { int sourceRate; int targetRate; };`
  - `class AudioResampler` con `void process(const int16_t* samples, size_t count, std::vector<int16_t>& out)` y `void reset()`.
  - `class SampleRing` con `explicit SampleRing(size_t capacity)`, `void push(const float* data, size_t count)`, `size_t size() const`, `bool pop(float* dst, size_t count)`, `void clear()`.

- [ ] **Step 1: Escribir la sonda que falla**

`labs/audio-probe/audio-probe.cc`:

```cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <vector>

namespace
{

int gFailures = 0;

void check(const char* what, bool got, bool want)
{
  const bool ok = got == want;
  if (!ok)
    gFailures++;
  std::printf("  [%s] %s (got=%d want=%d)\n", ok ? "OK" : "FAIL", what,
              static_cast<int>(got), static_cast<int>(want));
}

std::vector<int16_t> tone(int rate, double hz, double seconds)
{
  const int n = static_cast<int>(rate * seconds);
  std::vector<int16_t> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] =
        static_cast<int16_t>(12000.0 * std::sin(2.0 * M_PI * hz * i / rate));
  return out;
}

void resamplerCheck()
{
  std::printf("\n=== resampler ===\n");
  const auto sig = tone(8000, 500.0, 4.0);

  AudioResampler whole({.sourceRate = 8000, .targetRate = 16000});
  std::vector<int16_t> once;
  whole.process(sig.data(), sig.size(), once);

  AudioResampler streamed({.sourceRate = 8000, .targetRate = 16000});
  std::vector<int16_t> blocks;
  std::vector<int16_t> tmp;
  for (size_t i = 0; i + 1024 <= sig.size(); i += 1024) {
    streamed.process(sig.data() + i, 1024, tmp);
    blocks.insert(blocks.end(), tmp.begin(), tmp.end());
  }

  const size_t common = std::min(once.size(), blocks.size());
  size_t differing = 0;
  for (size_t i = 0; i < common; ++i)
    if (once[i] != blocks[i])
      differing++;

  check("streaming == una pasada", differing == 0, true);
  check("perdida por bloques < 1%",
        blocks.size() >= sig.size() * 2 * 99 / 100, true);

  AudioResampler down({.sourceRate = 44100, .targetRate = 8000});
  const auto voice = tone(44100, 440.0, 3.0);
  std::vector<int16_t> out8k;
  down.process(voice.data(), voice.size(), out8k);
  check("44.1k->8k conserva la duracion",
        out8k.size() > 23800 && out8k.size() < 24100, true);

  AudioResampler same({.sourceRate = 16000, .targetRate = 16000});
  std::vector<int16_t> passthrough;
  same.process(sig.data(), 512, passthrough);
  check("mismo ritmo es passthrough", passthrough.size() == 512, true);
}

void ringCheck()
{
  std::printf("\n=== sample ring ===\n");
  SampleRing ring(1024);
  std::vector<float> in(600, 0.5F);
  ring.push(in.data(), in.size());
  check("ring acumula", ring.size() == 600, true);

  std::vector<float> out(512);
  check("ring entrega un bloque", ring.pop(out.data(), 512), true);
  check("ring descuenta lo entregado", ring.size() == 88, true);
  check("ring no entrega de mas", ring.pop(out.data(), 512), false);

  ring.push(in.data(), in.size());
  ring.push(in.data(), in.size());
  check("ring acota la memoria", ring.size() <= 1024, true);
}

} // namespace

int main()
{
  resamplerCheck();
  ringCheck();
  std::printf("\n%s (%d fallos)\n", gFailures == 0 ? "TODO OK" : "HAY FALLOS",
              gFailures);
  return gFailures == 0 ? 0 : 1;
}
```

`labs/audio-probe/CMakeLists.txt`:

```cmake
add_executable(argus-audio-probe
    audio-probe.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
)
target_include_directories(argus-audio-probe PRIVATE ${SRC_ROOT})
target_compile_options(argus-audio-probe PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-audio-probe PRIVATE $<$<CONFIG:Release>:-flto=auto>)
```

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
cmake --preset dev && cmake --build --preset dev -j 8 --target argus-audio-probe
```

Esperado: no compila, `audio-resampler.hxx` no existe.

- [ ] **Step 3: Implementar `SampleRing`**

`src/shared/wrapper/audio/sample-ring.hxx`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

class SampleRing
{
public:
  explicit SampleRing(size_t capacity) : buffer_(capacity, 0.0F) {}

  void push(const float* data, size_t count)
  {
    if (count >= buffer_.size()) {
      std::copy(data + count - buffer_.size(), data + count, buffer_.begin());
      head_ = 0;
      size_ = buffer_.size();
      return;
    }
    for (size_t i = 0; i < count; ++i) {
      buffer_[(head_ + size_) % buffer_.size()] = data[i];
      if (size_ == buffer_.size())
        head_ = (head_ + 1) % buffer_.size();
      else
        ++size_;
    }
  }

  bool pop(float* dst, size_t count)
  {
    if (count > size_)
      return false;
    for (size_t i = 0; i < count; ++i)
      dst[i] = buffer_[(head_ + i) % buffer_.size()];
    head_ = (head_ + count) % buffer_.size();
    size_ -= count;
    return true;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return buffer_.size(); }
  void clear() { head_ = 0; size_ = 0; }

private:
  std::vector<float> buffer_;
  size_t head_{0};
  size_t size_{0};
};
```

Sustituye a los `vector::erase(begin(), begin()+512)` del camino de audio, que son O(n) por ventana sobre un buffer que crece mientras el LLM y el TTS trabajan.

- [ ] **Step 4: Implementar `AudioResampler`**

`src/shared/wrapper/audio/audio-resampler.hxx`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

struct AudioResamplerInput
{
  int sourceRate;
  int targetRate;
};

class AudioResampler
{
public:
  explicit AudioResampler(AudioResamplerInput input);

  void process(const int16_t* samples, size_t count, std::vector<int16_t>& out);
  void reset();

  int sourceRate() const { return sourceRate_; }
  int targetRate() const { return targetRate_; }

private:
  static constexpr int kSincHalf = 32;

  double tap(double t) const;
  int16_t sampleAt(double pos) const;

  int sourceRate_;
  int targetRate_;
  double ratio_;
  double cutoff_;
  double pos_{0.0};
  std::vector<int16_t> history_;
  std::vector<double> window_;
};
```

`src/shared/wrapper/audio/audio-resampler.cc`:

```cpp
#include "audio-resampler.hxx"

#include <algorithm>
#include <cmath>

AudioResampler::AudioResampler(AudioResamplerInput input)
    : sourceRate_(input.sourceRate), targetRate_(input.targetRate),
      ratio_(static_cast<double>(input.sourceRate) /
             static_cast<double>(input.targetRate)),
      cutoff_(0.45 * std::min(input.sourceRate, input.targetRate) /
              static_cast<double>(input.sourceRate))
{
  window_.resize(2 * kSincHalf + 1);
  for (int j = -kSincHalf; j <= kSincHalf; ++j) {
    const double n = static_cast<double>(j + kSincHalf);
    const double N = static_cast<double>(2 * kSincHalf);
    window_[static_cast<size_t>(j + kSincHalf)] =
        0.42 - 0.5 * std::cos(2.0 * M_PI * n / N) +
        0.08 * std::cos(4.0 * M_PI * n / N);
  }
  reset();
}

void AudioResampler::reset()
{
  history_.assign(kSincHalf, 0);
  pos_ = static_cast<double>(kSincHalf);
}

double AudioResampler::tap(double t) const
{
  if (std::fabs(t) < 1e-12)
    return 2.0 * cutoff_;
  return std::sin(2.0 * M_PI * cutoff_ * t) / (M_PI * t);
}

int16_t AudioResampler::sampleAt(double pos) const
{
  const size_t i0 = static_cast<size_t>(pos);
  const double fraction = pos - static_cast<double>(i0);
  double acc = 0.0;
  double wsum = 0.0;
  for (int j = -kSincHalf; j <= kSincHalf; ++j) {
    const long idx = static_cast<long>(i0) + j;
    if (idx < 0 || idx >= static_cast<long>(history_.size()))
      continue;
    const double weight =
        tap(static_cast<double>(j) - fraction) *
        window_[static_cast<size_t>(j + kSincHalf)];
    acc += static_cast<double>(history_[static_cast<size_t>(idx)]) * weight;
    wsum += weight;
  }
  const double value = wsum > 1e-9 ? acc / wsum : 0.0;
  return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
}

void AudioResampler::process(const int16_t* samples, size_t count,
                             std::vector<int16_t>& out)
{
  out.clear();
  if (count == 0)
    return;
  if (sourceRate_ == targetRate_) {
    out.assign(samples, samples + count);
    return;
  }

  history_.insert(history_.end(), samples, samples + count);
  while (pos_ + static_cast<double>(kSincHalf) <
         static_cast<double>(history_.size())) {
    out.push_back(sampleAt(pos_));
    pos_ += ratio_;
  }

  const size_t floorPos = static_cast<size_t>(pos_);
  const size_t keep = floorPos > static_cast<size_t>(kSincHalf)
                          ? floorPos - static_cast<size_t>(kSincHalf)
                          : 0;
  if (keep > 0) {
    history_.erase(history_.begin(), history_.begin() + static_cast<long>(keep));
    pos_ -= static_cast<double>(keep);
  }
}
```

El estado que faltaba son dos cosas: `history_` retiene las `kSincHalf` muestras que el sinc necesita mirar hacia atrás, y `pos_` conserva la fase fraccionaria entre llamadas. Con eso, procesar por bloques y procesar de una pasada dan **exactamente las mismas muestras**.

- [ ] **Step 5: Registrar la sonda**

En `CMakeLists.txt`, tras la línea 28:

```cmake
add_subdirectory(labs/audio-probe)
```

- [ ] **Step 6: Verificar**

```bash
cmake --preset dev && cmake --build --preset dev -j 8 --target argus-audio-probe
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: `TODO OK (0 fallos)`. La aserción clave es `streaming == una pasada`: 0 muestras distintas entre los dos caminos.

- [ ] **Step 7: Commit**

```bash
git add src/shared/wrapper/audio labs/audio-probe CMakeLists.txt
git commit -m "feat(audio): add stateful sinc resampler and fixed-capacity sample ring"
```

---

### Task 7: Enchufar el resampler y los rings en el camino de la cámara

**Files:**
- Modify: `src/shared/services/tapo/tapo-audio.hxx`
- Modify: `src/shared/services/tapo/tapo-audio.cc:55-105`
- Modify: `labs/voice-test/camera-audio.hxx`
- Modify: `labs/voice-test/camera-audio.cc`
- Modify: `labs/voice-test/voice-test.cc`
- Modify: `labs/voice-test/CMakeLists.txt`

**Interfaces:**
- Consumes: `AudioResampler`, `SampleRing` de la Task 6.
- Produces: `tapo_audio::resample(const TapoResampleInput&)` mantiene su firma (una pasada); `CameraMic` gana un `AudioResampler` miembro.

- [ ] **Step 1: Delegar `tapo_audio::resample`**

```cpp
std::vector<int16_t> resample(const TapoResampleInput& input)
{
  if (input.sourceRate == input.targetRate)
    return input.samples;
  AudioResampler resampler({.sourceRate = input.sourceRate,
                            .targetRate = input.targetRate});
  std::vector<int16_t> out;
  resampler.process(input.samples.data(), input.samples.size(), out);
  return out;
}
```

Una sola implementación de DSP en el proyecto. Las llamadas de una pasada (el canal de voz sintetiza la frase entera antes de enviarla) siguen funcionando igual.

- [ ] **Step 2: Dar estado a `CameraMic`**

En `camera-audio.hxx`, el `Impl` gana `std::unique_ptr<AudioResampler> resampler_;` — creado en `open()`, cuando ya se conoce `sourceRate`. En `readBlock()`:

```cpp
  std::vector<int16_t> up;
  impl_->resampler->process(chunk.data(), chunk.size(), up);
```

Y sustituir el `new`/`delete` de `Impl` por `std::unique_ptr<Impl>`: la regla del proyecto es no tener punteros propietarios crudos.

- [ ] **Step 3: Sustituir los buffers que se erosionan**

En `voice-test.cc`, `camBuf` es un `std::vector<float>` del que se hace `erase(begin, begin+512)` en cada ventana mientras crece libremente durante la respuesta del LLM. Pasa a `SampleRing`:

```cpp
  SampleRing camBuf(16000 * 30);
  std::array<float, 512> chunk{};
  ...
    bool haveChunk = false;
    {
      std::lock_guard<std::mutex> lock(bufMutex);
      haveChunk = camBuf.pop(chunk.data(), chunk.size());
    }
```

La capacidad fija de 30 s también acota la memoria: si nadie consume, se descarta lo más viejo, que para audio en vivo es exactamente lo que se quiere.

- [ ] **Step 4: Verificar la mejora de forma medible**

Añadir a `labs/voice-test` un modo `--audio-dump <fichero.wav>` que capture 8 s del micrófono de la cámara y escriba el WAV a 16 kHz. Comparar antes/después:

```bash
./build/prod/labs/voice-test/argus-voice-test --audio-dump before.wav
ffprobe -v error -show_entries format=duration -of csv=p=0 before.wav
```

Esperado tras el cambio: la duración del WAV coincide con los 8 s de captura (antes salía ~6% corta) y el espectrograma no muestra los cortes periódicos cada 128 ms.

- [ ] **Step 5: Verificar STT sobre el mismo audio**

```bash
./build/prod/labs/voice-test/argus-voice-test --camera-stt-check
```

Repetir 3 veces la misma frase y anotar la transcripción. Esperado: transcripción estable y sin palabras inventadas, que es lo que el troceado impedía.

- [ ] **Step 6: Commit**

```bash
git add src/shared/services/tapo labs/voice-test
git commit -m "fix(audio): resample the camera microphone with carried state"
```

---

### Task 8: Sesión de voz persistente hacia la cámara

`speakToCamera()` construye un `TapoTalkClient`, llama a `open()` — conexión TCP, Digest con sondeo de variantes de contraseña, `Key-Exchange`, arranque de sesión `talk` — envía **una frase** y cierra. En el camino de streaming eso es un handshake completo por frase, que anula justo la mejora de latencia por la que se trocea por frases. Además el muxer TS reinicia el PTS en cada frase.

**Files:**
- Modify: `src/shared/services/tapo/tapo-talk-client.hxx`
- Modify: `src/shared/services/tapo/tapo-talk-client.cc`
- Modify: `labs/voice-test/voice-test.cc`
- Modify: `config.toml` sección `[tapo]`

**Interfaces:**
- Consumes: `TapoTalkClient::open/send/close` existentes.
- Produces:
  - `struct TapoTalkSendInput { std::vector<int16_t> samples; int sampleRate; bool reopenOnFailure; };`
  - `TapoResult TapoTalkClient::sendChunk(const TapoTalkSendInput& input, const CancellationToken& token);`
  - `int64_t TapoTalkClient::sentDurationMs() const;` — usado por el control de eco de la Task 11.

- [ ] **Step 1: Medir el coste actual**

Instrumentar `speakToCamera` con un cronómetro alrededor de `client.open()` y ejecutar una respuesta de 3 frases:

```bash
./build/prod/labs/voice-test/argus-voice-test --camera 2>&1 | grep 'talk open'
```

Repetir 3 veces y anotar la mediana. Ese tiempo, multiplicado por el número de frases, es lo que esta tarea elimina.

- [ ] **Step 2: Añadir `sendChunk` con reapertura**

```cpp
TapoResult TapoTalkClient::sendChunk(const TapoTalkSendInput& input,
                                     const CancellationToken& token)
{
  if (!open_) {
    const auto opened = open();
    if (!opened.ok)
      return opened;
  }

  auto result = send({.samples = input.samples, .sampleRate = input.sampleRate},
                     token);
  if (result.ok || !input.reopenOnFailure)
    return result;

  close();
  const auto reopened = open();
  if (!reopened.ok)
    return reopened;
  return send({.samples = input.samples, .sampleRate = input.sampleRate}, token);
}
```

El contador `pts90k_` y el `muxer_` siguen siendo miembros, así que la continuidad temporal se mantiene entre frases de una misma respuesta.

- [ ] **Step 3: Exponer la duración realmente enviada**

El control de eco de la Task 11 necesita saber cuánto audio se ha mandado, no cuánto se pidió mandar (una cancelación corta el envío a mitad). `send()` ya trocea en paquetes de `packetMs`: basta con acumular lo que sale.

```cpp
int64_t TapoTalkClient::sentDurationMs() const
{
  return sentSamples_ * 1000 / kTargetSampleRate;
}
```

`sentSamples_` es un miembro `int64_t` que `send()` incrementa por cada paquete escrito con éxito y que `open()` pone a cero. Declararlo junto a `pts90k_` en la cabecera.

- [ ] **Step 4: Un cliente por conversación**

En `voice-test.cc`, el `TapoTalkClient` sube al ámbito del bucle de conversación como miembro del estado, no como local de `speakToCamera`:

```cpp
  TapoTalkClient talkClient(input.talkConfig);
  ...
      talkClient.sendChunk({.samples = s16,
                            .sampleRate = TtsService::sampleRate(),
                            .reopenOnFailure = true},
                           token);
```

- [ ] **Step 5: Configurar el keepalive**

En `config.toml`, sección `[tapo]`:

```toml
# Segundos sin audio tras los que se cierra la sesión de voz. La cámara la
# corta por su cuenta pasado un tiempo; reabrirla cuesta un handshake entero,
# así que conviene mantenerla mientras la conversación esté viva.
talk_idle_timeout_s = 60
```

- [ ] **Step 6: Verificar**

```bash
./build/prod/labs/voice-test/argus-voice-test --camera 2>&1 | grep 'talk open'
```

Esperado: **un solo** `talk open` por conversación en vez de uno por frase, y el hueco entre frases reducido al tiempo de síntesis.

- [ ] **Step 7: Commit**

```bash
git add src/shared/services/tapo labs/voice-test config.toml
git commit -m "perf(tapo): keep the talk session open across sentences"
```

---

## Fase 3 — VAD

### Task 9: Corregir el contexto de Silero y quitar el coste por ventana

Silero v5 espera que las 64 muestras de contexto sean **las inmediatamente anteriores** a la ventana. El código copia `pending_.end() - 64`, es decir la cola de *todo* lo pendiente: coincide por casualidad cuando se alimenta de 512 en 512 y deja de coincidir en cuanto llega un bloque mayor — que es exactamente lo que pasa al conectar el micrófono de la cámara. Además cada ventana de 32 ms asigna un `vector` de 576 floats, otro de shapes y otro de feeds, y toma un mutex que no hace falta.

**Files:**
- Modify: `labs/voice-test/vad.hxx`
- Modify: `labs/voice-test/vad.cc`
- Modify: `labs/audio-probe/audio-probe.cc`
- Modify: `labs/audio-probe/CMakeLists.txt`

**Interfaces:**
- Consumes: `SampleRing` de la Task 6.
- Produces: `Vad::process` da el mismo resultado sea cual sea el tamaño de bloque con que se alimente.

- [ ] **Step 1: Escribir la aserción que falla**

En `labs/audio-probe/audio-probe.cc`:

```cpp
void vadCheck()
{
  std::printf("\n=== vad ===\n");
  std::vector<float> speech;
  const auto pcm = tone(16000, 220.0, 1.5);
  for (const auto s : pcm)
    speech.push_back(static_cast<float>(s) / 32768.0F);
  std::vector<float> signal(16000, 0.0F);
  signal.insert(signal.end(), speech.begin(), speech.end());
  signal.insert(signal.end(), 16000, 0.0F);

  const auto probs = [&](size_t block) {
    Vad vad;
    std::vector<float> turn;
    std::vector<float> out;
    for (size_t i = 0; i + block <= signal.size(); i += block) {
      vad.process(signal.data() + i, static_cast<int>(block), turn);
      out.push_back(vad.lastProb());
    }
    return out;
  };

  const auto a = probs(512);
  const auto b = probs(1920);
  size_t compared = 0;
  size_t equal = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    compared++;
    if (std::fabs(a[i] - b[i]) < 1e-4F)
      equal++;
  }
  check("VAD independiente del tamano de bloque", equal == compared, true);
}
```

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
cmake --build --preset dev -j 8 --target argus-audio-probe
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: `VAD independiente del tamano de bloque` falla, porque con bloques de 1920 el contexto sale de la posición equivocada.

- [ ] **Step 3: Corregir el contexto**

En `vad.cc`, sustituir la línea 85 por:

```cpp
    std::copy(pending_.begin() + kWindowSize - kContextSize,
              pending_.begin() + kWindowSize, context_.begin());
```

- [ ] **Step 4: Eliminar las asignaciones por ventana**

Los buffers pasan a miembros creados una vez en el constructor:

```cpp
  std::vector<float> window_;
  std::array<int64_t, 1> sampleRateInput_{16000};
  std::array<int64_t, 2> inputShape_{1, kEffectiveWindow};
  std::array<int64_t, 3> stateShape_{2, 1, 128};
  std::array<int64_t, 1> srShape_{1};
```

y `runModel` reutiliza `window_` y construye los `Ort::Value` sobre los miembros. A 31 ventanas por segundo eso son ~124 asignaciones/s menos en el camino de tiempo real.

- [ ] **Step 5: Quitar el mutex y el estado muerto**

`Vad` es un objeto por stream de audio: dos hilos no comparten una instancia. Se elimina `mutex_` y se elimina `hasSpeech_`, que se escribe y nunca se lee.

- [ ] **Step 6: Sustituir `pending_` por `SampleRing`**

`pending_.erase(pending_.begin(), pending_.begin() + kWindowSize)` es un memmove por ventana. Con `SampleRing` de capacidad `kWindowSize * 8` el consumo es O(1) y el buffer no puede crecer.

- [ ] **Step 7: Verificar**

```bash
cmake --build --preset dev -j 8 --target argus-audio-probe
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: `VAD independiente del tamano de bloque` en OK.

- [ ] **Step 8: Commit**

```bash
git add labs/voice-test/vad.hxx labs/voice-test/vad.cc labs/audio-probe
git commit -m "fix(vad): use the adjacent context window and drop per-window allocations"
```

---

### Task 10: Puerta de calidad de turno

Whisper alucina con audio corto o casi silencioso: devuelve muletillas y frases hechas que luego entran al LLM como si el usuario las hubiera dicho. Hoy cualquier turno que supere `minSpeechFrames` (5 ventanas = 160 ms) se transcribe. Y `minSilenceFrames = 8` cierra el turno con 256 ms de silencio, que en español conversacional parte frases por la mitad.

**Files:**
- Modify: `labs/voice-test/vad.hxx`
- Modify: `labs/voice-test/vad.cc`
- Modify: `config.toml` (sección `[vad]` nueva)
- Modify: `labs/audio-probe/audio-probe.cc`

**Interfaces:**
- Consumes: `ConfigService`.
- Produces:
  - `struct VadTurn { std::vector<float> samples; int speechFrames; float meanProb; };`
  - `bool Vad::process(const float* samples, int count, VadTurn& outTurn);` — devuelve `true` solo si el turno **supera la puerta**.

- [ ] **Step 1: Escribir la aserción que falla**

```cpp
void vadGateCheck()
{
  std::printf("\n=== vad gate ===\n");

  std::vector<float> blip(16000, 0.0F);
  const auto pcm = tone(16000, 300.0, 0.12);
  for (size_t i = 0; i < pcm.size(); ++i)
    blip[8000 + i] = static_cast<float>(pcm[i]) / 32768.0F;
  blip.insert(blip.end(), 16000, 0.0F);

  Vad vad;
  VadTurn turn;
  bool fired = false;
  for (size_t i = 0; i + 512 <= blip.size(); i += 512)
    if (vad.process(blip.data() + i, 512, turn))
      fired = true;
  check("un blip de 120ms no genera turno", fired, false);

  std::vector<float> utterance(8000, 0.0F);
  const auto voice = tone(16000, 220.0, 1.2);
  for (const auto s : voice)
    utterance.push_back(static_cast<float>(s) / 32768.0F);
  utterance.insert(utterance.end(), 16000, 0.0F);

  Vad vad2;
  VadTurn turn2;
  bool fired2 = false;
  for (size_t i = 0; i + 512 <= utterance.size(); i += 512)
    if (vad2.process(utterance.data() + i, 512, turn2))
      fired2 = true;
  check("1.2s de voz si genera turno", fired2, true);
  check("el turno reporta su probabilidad media", turn2.meanProb > 0.0F, true);
}
```

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: no compila (`VadTurn` no existe).

- [ ] **Step 3: Añadir la puerta**

En `vad.hxx`, ampliar `Config`:

```cpp
    int minTurnMs{320};
    float minMeanProb{0.55F};
```

y en `vad.cc`, acumular durante el turno:

```cpp
    if (speech_) {
      buffer_.insert(buffer_.end(), window_.begin() + kContextSize, window_.end());
      speechProbSum_ += prob;
      frameCounter_++;
    }
```

y al cerrar:

```cpp
    if (speech_ && (silenceCounter_ >= cfg_.minSilenceFrames ||
                    frameCounter_ >= cfg_.maxTurnFrames)) {
      const float meanProb =
          frameCounter_ > 0 ? speechProbSum_ / static_cast<float>(frameCounter_)
                            : 0.0F;
      const int speechMs = frameCounter_ * kWindowSize * 1000 / cfg_.sampleRate;
      const bool accepted =
          speechMs >= cfg_.minTurnMs && meanProb >= cfg_.minMeanProb;
      if (accepted) {
        outTurn.samples = std::move(buffer_);
        outTurn.speechFrames = frameCounter_;
        outTurn.meanProb = meanProb;
        completed = true;
      }
      buffer_.clear();
      preRoll_.clear();
      speech_ = false;
      startCounter_ = 0;
      silenceCounter_ = 0;
      frameCounter_ = 0;
      speechProbSum_ = 0.0F;
    }
```

- [ ] **Step 4: Llevar los umbrales a `config.toml`**

```toml
# ── Detección de voz (Silero VAD v5) ──────────────────────────────────
# El micrófono de la cámara es de campo lejano y con más ruido que uno de
# diadema, así que el umbral de arranque es bajo y quien filtra los falsos
# positivos es la puerta de calidad de turno, no el umbral.
[vad]
threshold = 0.45
neg_threshold = 0.25
# Ventanas de 32 ms de voz para abrir un turno.
min_speech_frames = 5
# Ventanas de 32 ms de silencio para cerrarlo. 12 = 384 ms: por debajo de
# 300 ms se parten las frases en español por la pausa entre sintagmas.
min_silence_frames = 12
# Tope duro de un turno (750 ventanas = 24 s).
max_turn_frames = 750
# Ventanas guardadas antes del inicio detectado para no cortar el fonema
# inicial. 10 = 320 ms.
pre_roll_frames = 10
# Puerta de calidad: por debajo de esto el turno se descarta sin llamar al
# STT. Whisper inventa frases cuando le llega audio corto o casi silencioso.
min_turn_ms = 320
min_mean_prob = 0.55
```

`Vad::Config` se rellena desde `ConfigService` en el constructor, con los valores de arriba como defecto cuando la clave no existe.

- [ ] **Step 5: Adaptar los llamantes**

`voice-test.cc` pasa de `std::vector<float> turn` a `VadTurn turn` y transcribe `turn.samples`. Con la puerta activa, el bloque `onlySpaces` que descartaba transcripciones vacías deja de ser el único filtro.

- [ ] **Step 6: Verificar**

```bash
cmake --build --preset dev -j 8 --target argus-audio-probe
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: las tres aserciones de `=== vad gate ===` en OK.

- [ ] **Step 7: Commit**

```bash
git add labs/voice-test labs/audio-probe config.toml
git commit -m "feat(vad): gate turns by speech duration and mean probability"
```

---

### Task 11: Calibración con audio real y control de eco cerrado

Los umbrales del VAD se han movido dos veces a ojo (`0.5 → 0.4`, `16 → 8` ventanas de silencio) sin una medición que los respalde. Y el eco del altavoz se ataca hoy con un descarte fijo de 600 ms que no tiene relación con la duración de lo que se acaba de decir.

**Files:**
- Modify: `labs/audio-probe/audio-probe.cc`
- Modify: `labs/audio-probe/CMakeLists.txt`
- Modify: `labs/voice-test/voice-test.cc`
- Modify: `config.toml` sección `[tapo]`
- Create: `labs/fixtures/README.md`

**Interfaces:**
- Consumes: `TapoTalkClient::sentDurationMs()` de la Task 8, `Vad` de la Task 10.
- Produces: `argus-audio-probe --vad <fichero.wav>` imprime el informe de calibración.

- [ ] **Step 1: Grabar los fixtures**

```bash
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-quiet.wav
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-speech.wav
./build/prod/labs/voice-test/argus-voice-test --audio-dump labs/fixtures/camera-echo.wav
```

`camera-quiet` es la habitación en silencio, `camera-speech` tres frases con pausas naturales entre ellas, `camera-echo` una respuesta de Argus grabada desde el propio micrófono de la cámara. Documentar en `labs/fixtures/README.md` cómo se grabó cada uno; los WAV se versionan porque son la base de la calibración.

- [ ] **Step 2: Añadir el modo de calibración**

```cpp
void vadReport(const std::string& path)
{
  std::vector<float> samples;
  if (!readWav16k(path, samples)) {
    std::printf("no se pudo leer %s\n", path.c_str());
    return;
  }

  Vad vad;
  VadTurn turn;
  int windows = 0;
  int turns = 0;
  float maxProb = 0.0F;
  double probSum = 0.0;
  std::vector<int> turnMs;
  for (size_t i = 0; i + 512 <= samples.size(); i += 512) {
    const bool fired = vad.process(samples.data() + i, 512, turn);
    windows++;
    maxProb = std::max(maxProb, vad.lastProb());
    probSum += vad.lastProb();
    if (fired) {
      turns++;
      turnMs.push_back(static_cast<int>(turn.samples.size() * 1000 / 16000));
    }
  }

  std::printf("%s: %.2fs ventanas=%d turnos=%d probMedia=%.3f probMax=%.3f\n",
              path.c_str(), samples.size() / 16000.0, windows, turns,
              probSum / std::max(1, windows), maxProb);
  for (size_t i = 0; i < turnMs.size(); ++i)
    std::printf("  turno %zu: %d ms\n", i + 1, turnMs[i]);
}
```

- [ ] **Step 3: Fijar los umbrales con datos**

```bash
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-quiet.wav
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-speech.wav
```

Criterio de aceptación, y **hay que anotar los números obtenidos en `CONTEXT.md`**:
- `camera-quiet`: **0 turnos**. Si sale alguno, subir `min_mean_prob` hasta que no salga.
- `camera-speech`: **exactamente 3 turnos**, uno por frase. Más turnos significa `min_silence_frames` demasiado bajo (frases partidas); menos significa demasiado alto (frases fusionadas).

- [ ] **Step 4: Cerrar el lazo del eco**

Sustituir la constante `kEchoDrainSamples` por un descarte derivado de lo que realmente se envió:

```cpp
    const int64_t drainMs = talkClient.sentDurationMs() + talkDrainMarginMs_;
    discardRemaining.store(
        static_cast<int>(drainMs * 16000 / 1000));
    vad.reset();
```

`vad.reset()` es importante: sin él, el estado LSTM de Silero cruza el hueco de la reproducción y arranca el turno siguiente con la memoria contaminada por el eco.

En `config.toml`:

```toml
# Margen sobre la duración del audio enviado antes de volver a escuchar. El
# altavoz de la cámara sigue sonando después de que el último paquete salga
# del backend; calibrar con labs/fixtures/camera-echo.wav.
talk_drain_margin_ms = 400
```

- [ ] **Step 5: Calibrar el margen**

```bash
./build/prod/labs/audio-probe/argus-audio-probe --vad labs/fixtures/camera-echo.wav
```

Buscar el instante en que la probabilidad de voz vuelve por debajo de `neg_threshold` tras el final del audio enviado: ese retardo, redondeado hacia arriba, es `talk_drain_margin_ms`.

- [ ] **Step 6: Verificar de extremo a extremo**

```bash
./build/prod/labs/voice-test/argus-voice-test --camera
```

Esperado: Argus no se auto-interrumpe con su propia voz, y una frase con pausas naturales llega entera al STT en un único turno. Repetir 3 veces.

- [ ] **Step 7: Commit**

```bash
git add labs/audio-probe labs/fixtures labs/voice-test config.toml
git commit -m "feat(vad): calibrate thresholds against recorded camera audio and close the echo loop"
```

---

## Fase 4 — Arquitectura

### Task 12: Subir el VAD a `src/shared/services/vad/`

El VAD ya no es un prototipo: es la puerta de entrada de la conversación. Vive en `labs/voice-test/` y de ahí no lo puede usar el backend. A diferencia de `LlmService`, `SttService` o `TtsService` — que son estáticos porque hay un solo contexto de inferencia compartido — el VAD **tiene estado por stream de audio**, así que debe ser una clase de instancia, como `JwtService` o `RoomManager`.

**Files:**
- Create: `src/shared/services/vad/vad-service.hxx`
- Create: `src/shared/services/vad/vad-service.cc`
- Delete: `labs/voice-test/vad.hxx`, `labs/voice-test/vad.cc`
- Modify: `labs/voice-test/CMakeLists.txt`
- Modify: `labs/voice-test/voice-test.cc`
- Modify: `labs/audio-probe/CMakeLists.txt`

**Interfaces:**
- Consumes: `SampleRing`, `ConfigService`, ONNX Runtime.
- Produces:
  - `struct VadConfig { int sampleRate; float threshold; float negThreshold; int minSpeechFrames; int minSilenceFrames; int maxTurnFrames; int preRollFrames; int minTurnMs; float minMeanProb; };`
  - `struct VadTurn { std::vector<float> samples; int speechFrames; float meanProb; };`
  - `class VadService` con `VadService()`, `explicit VadService(const VadConfig&)`, `bool process(const float* samples, int count, VadTurn& outTurn)`, `bool inSpeech() const`, `float lastProb() const`, `void reset()`, `static bool isLoaded()`.

- [ ] **Step 1: Mover y renombrar**

```bash
git mv labs/voice-test/vad.hxx src/shared/services/vad/vad-service.hxx
git mv labs/voice-test/vad.cc src/shared/services/vad/vad-service.cc
```

Renombrar la clase `Vad` → `VadService` y `Vad::Config` → `VadConfig` (struct a nivel de fichero, como el resto de structs de parámetros del proyecto).

- [ ] **Step 2: Compartir la sesión ONNX**

El modelo son 2,3 MB y cargarlo por instancia es un desperdicio si hay varias conversaciones. La sesión y el `Ort::Env` pasan a ser estáticos del fichero, y **solo el estado LSTM es por instancia**:

```cpp
namespace
{

Ort::Session& vadSession()
{
  static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "Argus-Vad");
  static Ort::Session session = [&] {
    auto opts = Ort::SessionOptions{};
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(1);
    opts.SetInterOpNumThreads(1);
    return Ort::Session(env, kModelPath, opts);
  }();
  return session;
}

std::mutex& vadSessionMutex()
{
  static std::mutex mutex;
  return mutex;
}

} // namespace
```

`Session::Run` sí necesita el mutex ahora, porque la sesión se comparte entre instancias — es el mismo patrón de contexto compartido que ya usan `LlmService` y `TtsService`. Un hilo por conversación tomando el mutex 31 veces por segundo durante ~0,1 ms no es contención relevante.

- [ ] **Step 3: Leer la configuración**

El constructor por defecto rellena `VadConfig` desde `ConfigService` (sección `[vad]` de la Task 10); el constructor con `VadConfig` explícito sirve para las sondas, que deben ser reproducibles sin depender del fichero.

- [ ] **Step 4: Actualizar los CMakeLists**

`labs/voice-test/CMakeLists.txt` y `labs/audio-probe/CMakeLists.txt` pasan a listar `${SRC_ROOT}/shared/services/vad/vad-service.cc`. El binario del backend lo recoge solo, porque `SHARED_SRC` es un `GLOB_RECURSE` sobre `src/shared/**/*.cc`.

- [ ] **Step 5: Verificar**

```bash
cmake --preset dev && cmake --build --preset dev -j 8
./build/dev/labs/audio-probe/argus-audio-probe
```

Esperado: build limpio de los 16 binarios y todas las aserciones de la sonda de audio en OK. El VAD se comporta igual, solo ha cambiado de sitio.

- [ ] **Step 6: Commit**

```bash
git add src/shared/services/vad labs
git commit -m "refactor(vad): promote the VAD to a shared instance service"
```

---

### Task 13: Subir la fuente de audio de la cámara a `src/`

`CameraMic` decodifica RTSP con FFmpeg dentro de `labs/`, y FFmpeg no es dependencia del backend. Hay dos caminos y **la decisión hay que tomarla antes de escribir código**:

- **A — FFmpeg en el backend.** `libavformat`/`libavcodec` por `pkg-config`, como en el lab. Directo, pero mete una dependencia de sistema grande y una segunda ruta de acceso a la cámara, en paralelo a go2rtc.
- **B — go2rtc también para el audio.** go2rtc ya mantiene la conexión RTSP, ya está supervisado por `Go2rtcManager` y ya sirve `/api/stream.mp4`. Basta con pedirle una salida de audio y consumirla con el mismo `upstream_http` que usa `StreamHub`. No añade dependencias, reutiliza el broker que ya existe y deja una sola conexión hacia la cámara.

**B es coherente con la arquitectura**: go2rtc es el broker de medios del sistema y el backend no debería abrir sus propias sesiones RTSP en paralelo. A queda como plan de contingencia si go2rtc no puede entregar PCM de la forma que se necesita.

**Files:**
- Create: `src/shared/services/stream/camera-audio-source.hxx`
- Create: `src/shared/services/stream/camera-audio-source.cc`
- Modify: `labs/voice-test/camera-audio.{hxx,cc}` (pasa a ser un envoltorio delgado o desaparece)
- Modify: `labs/stream-probe/stream-probe.cc`

**Interfaces:**
- Consumes: `Go2rtcManager::apiBase()`, `upstream_http::open`, `AudioResampler`, `SampleRing`.
- Produces:
  - `struct CameraAudioInput { int64_t cameraId; int targetRate; size_t ringCapacity; };`
  - `class CameraAudioSource` con `explicit CameraAudioSource(const CameraAudioInput&)`, `bool open()`, `bool read(std::vector<float>& out)`, `void close()`, `bool isOpen() const`.

- [ ] **Step 1: Comprobar qué entrega go2rtc**

```bash
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' \
  'http://127.0.0.1:1984/api/stream.mp4?src=cam1&audio=pcma'
```

Anotar el resultado. Si go2rtc entrega PCMA/PCM por HTTP, el camino B es viable tal cual y esta tarea sigue. Si devuelve un error, **parar y volver al camino A**, dejando anotado en `CONTEXT.md` qué se probó y por qué se descartó B — la próxima sesión no debería repetir la investigación.

- [ ] **Step 2: Escribir la aserción que falla**

En `labs/stream-probe/stream-probe.cc`, tras `streamHubCheck()`:

```cpp
void cameraAudioCheck()
{
  std::printf("\n=== camera audio ===\n");
  if (!std::getenv("ARGUS_PROBE_RTSP")) {
    std::printf("  omitido (define ARGUS_PROBE_RTSP)\n");
    return;
  }

  CameraAudioSource source({.cameraId = 1,
                            .targetRate = 16000,
                            .ringCapacity = 16000 * 30});
  check("audio de camara abre", source.open(), true);

  std::vector<float> block;
  size_t total = 0;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4)) {
    if (source.read(block))
      total += block.size();
  }
  source.close();

  check("entrega ~4s de audio a 16kHz",
        total > 16000 * 3 && total < 16000 * 5, true);
}
```

La aserción de duración es la que detecta la regresión que costó descubrir: un resampler sin estado entrega ~6% menos muestras de las que corresponden al tiempo transcurrido.

- [ ] **Step 3: Implementar `CameraAudioSource`**

Sigue el patrón de `StreamHub::runUpstream`: `upstream_http::open` contra go2rtc, lectura bloqueante con `SO_RCVTIMEO`, decodificación del formato de audio negociado en el Step 1, `AudioResampler` miembro hacia 16 kHz y `SampleRing` de salida. Todos los recursos con `std::unique_ptr`; ningún puntero propietario crudo.

- [ ] **Step 4: Verificar**

```bash
cmake --build --preset dev -j 8
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: las dos aserciones de `=== camera audio ===` en OK.

- [ ] **Step 5: Commit**

```bash
git add src/shared/services/stream labs
git commit -m "feat(stream): read camera audio through go2rtc instead of a second rtsp session"
```

---

## Fase 5 — Documentación

### Task 14: Actualizar `CONTEXT.md` y `AGENTS.md`

**Files:**
- Modify: `CONTEXT.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: los resultados medidos de las tareas 6, 7, 8 y 11.
- Produces: memoria del proyecto al día.

- [ ] **Step 1: Corregir lo que ya está obsoleto en `CONTEXT.md`**

La sección «Performance & portability batch» sigue describiendo **SmolVLM2 por ONNX** y **llama.cpp por Conan**, y las dos cosas están superadas por las secciones de 2026-08-08. Reescribir esos dos bullets para que apunten a la realidad actual (LFM2.5-VL por libmtmd, llama.cpp como submódulo `b10305`) en vez de dejar dos versiones contradictorias en el mismo fichero.

El mismo párrafo describe el VAD con «0.5 start / 0.35 end, ~250 ms min speech, ~500 ms silencio»: sustituir por los valores calibrados en la Task 11, **con los números de la calibración**.

- [ ] **Step 2: Documentar lo aprendido en esta ronda**

Añadir una sección nueva con, como mínimo:

- Por qué el resampler necesita estado, con la cifra: por bloques de 1024 a 8→16 kHz se perdía el 6,25% de las muestras y aparecía un corte cada 128 ms. Y la propiedad que lo verifica: streaming y una pasada dan muestras idénticas.
- Por qué el encoding HTTP se lee de las cabeceras y no se olfatea, con los dos modos de fallo observados: pérdida silenciosa de un `recv` sin `0x0A` y falso positivo de chunked que mata el flujo entero.
- Por qué el upstream muerto debe salir del mapa, y por qué se recolecta con `join` en el siguiente `getOrOpen` en vez de con `detach` (la regla del proyecto es `std::thread` + `join`).
- Por qué la ventana de créditos es por conexión: el cuello es el socket TCP compartido, no la suscripción.
- Por qué `VadService` es clase de instancia mientras el resto de servicios de IA son estáticos: el estado LSTM es por stream, la sesión ONNX se comparte.
- Por qué la sesión de voz se mantiene abierta entre frases, con el coste medido del handshake.
- El resultado de la investigación del Step 1 de la Task 13 (go2rtc para audio: sí o no, y por qué).

- [ ] **Step 3: Actualizar `AGENTS.md`**

- Sección 13d: dice que `VisionService` corre **SmolVLM2 (ONNX)**; ya corre LFM2.5-VL por libmtmd. Corregir.
- Tabla de ficheros clave: añadir `src/shared/wrapper/audio/`, `src/shared/services/vad/`, `labs/audio-probe/` y `STABILITY_AND_REALTIME_PLAN.md`; el `OPTIMIZATION_AND_MEMORY_PLAN.md` sigue siendo la fuente de verdad de las fases C-H.
- Añadir a las reglas la invariante de orden de locks del `StreamHub` (`hubMutex_` → `Upstream::mtx`) y la regla de que todo audio que se procese por bloques usa `AudioResampler`, nunca una conversión propia.

- [ ] **Step 4: Verificar**

```bash
grep -n "SmolVLM2" AGENTS.md CONTEXT.md
```

Esperado: solo apariciones históricas explicando la migración, ninguna describiendo el estado actual.

- [ ] **Step 5: Commit**

```bash
git add CONTEXT.md AGENTS.md
git commit -m "docs: record the audio, stream and vad findings and refresh stale sections"
```

---

---

### Task 15: Analizar la cámara sin abrir una segunda sesión RTSP

**Este es el bug reportado en producción**: tras pedir "analiza la cámara 1", la conversación se degrada — Argus escucha y habla cosas que no tocan. Tres causas encadenadas, todas en `describeCamera()`:

1. `captureCameraFrame()` abre una **tercera** conexión a la cámara (micrófono en el substream + canal de voz en el 8800 + esta en el principal). La C225 limita sesiones concurrentes: la del micrófono puede caerse, y el hilo del micrófono entra en su bucle de reapertura.
2. Bloquea el hilo de la conversación entre 1 y 3 s (`avformat_open_input` sobre RTSP + `find_stream_info` + decodificar hasta el primer frame).
3. Mientras tanto **nadie vacía el buffer del micrófono**. Al volver, el VAD procesa de golpe varios segundos de audio grabado *antes* y *durante* el análisis, y dispara turnos de algo que ya pasó.

`MediaRelay::snapshot()` ya resuelve el punto 1 y el 2: go2rtc mantiene la conexión a la cámara y `/api/frame.jpeg` devuelve un JPEG al instante, sin tocar la cámara ni añadir sesiones.

**Files:**
- Modify: `src/shared/services/stream/media-relay.hxx`
- Modify: `src/shared/services/stream/media-relay.cc`
- Modify: `labs/voice-test/voice-test.cc`
- Modify: `labs/voice-test/CMakeLists.txt`
- Test: `labs/stream-probe/stream-probe.cc`

**Interfaces:**
- Consumes: `Go2rtcManager::init/addSource/isRunning`, `VisionService::describeMat`.
- Produces: `static std::string MediaRelay::snapshotBytes(int64_t cameraId);` — JPEG crudo, cadena vacía si falla. `MediaRelay::snapshot()` pasa a construir su respuesta HTTP sobre ella.

- [ ] **Step 1: Escribir la aserción que falla**

En `labs/stream-probe/stream-probe.cc`, dentro de `streamHubCheck()` justo después de crear la fuente:

```cpp
  const std::string jpeg = MediaRelay::snapshotBytes(1);
  check("snapshot devuelve bytes", !jpeg.empty(), true);
  check("snapshot es un JPEG",
        jpeg.size() > 2 && static_cast<unsigned char>(jpeg[0]) == 0xFF &&
            static_cast<unsigned char>(jpeg[1]) == 0xD8, true);
```

Añadir `${SRC_ROOT}/shared/services/stream/media-relay.cc` a `labs/stream-probe/CMakeLists.txt`.

- [ ] **Step 2: Ejecutar y ver fallar**

```bash
cmake --build --preset dev -j 8 --target argus-stream-probe
ARGUS_PROBE_RTSP="rtsp://user:pass@camara:554/stream1" ./build/dev/labs/stream-probe/argus-stream-probe
```

Esperado: no compila, `snapshotBytes` no existe.

- [ ] **Step 3: Extraer `snapshotBytes`**

Sacar el cuerpo de `MediaRelay::snapshot()` a una función que devuelva los bytes, y dejar `snapshot()` como el envoltorio HTTP:

```cpp
std::string MediaRelay::snapshotBytes(int64_t cameraId)
{
  if (!Go2rtcManager::isRunning())
    return {};

  const auto [host, port] =
      upstream_http::splitHostPort(Go2rtcManager::apiBase().substr(7));
  const std::string path =
      "/api/frame.jpeg?src=" + Go2rtcManager::streamName(cameraId);

  upstream_http::Upstream up = upstream_http::open(host, port, path, 5);
  if (!up.ok)
    return {};

  std::string body = std::move(up.leftover);
  char tmp[16384];
  for (;;) {
    const auto n = ::recv(up.fd, tmp, sizeof(tmp), 0);
    if (n <= 0)
      break;
    body.append(tmp, static_cast<size_t>(n));
    if (body.size() > 8u * 1024u * 1024u)
      break;
  }
  ::close(up.fd);
  bytesRelayed_.fetch_add(static_cast<int64_t>(body.size()),
                          std::memory_order_relaxed);
  return body;
}
```

`snapshot()` queda en cuatro líneas: llama a `snapshotBytes`, devuelve 503 si go2rtc no corre, 502 si viene vacío, y `image/jpeg` con el cuerpo en el caso bueno.

- [ ] **Step 4: Arrancar go2rtc en el lab**

`voice-test` deja de hablar RTSP por su cuenta. En el arranque, tras `ConfigService::load`:

```cpp
  Go2rtcManager::init();
  if (!camRtspMain.empty())
    Go2rtcManager::addSource({.name = ConfigService::getString("voice_test.camera_name"),
                              .url = camRtspMain});
```

y `Go2rtcManager::shutdown()` en la salida, junto a los `shutdown()` de los servicios. Con esto el lab usa el mismo camino de medios que el backend, que es lo que manda la arquitectura, y la cámara pasa de tres conexiones a dos.

- [ ] **Step 5: Sustituir la captura de frame**

```cpp
cv::Mat captureCameraFrame(int64_t cameraId)
{
  const std::string jpeg = MediaRelay::snapshotBytes(cameraId);
  if (jpeg.empty())
    return {};
  const std::vector<uchar> buffer(jpeg.begin(), jpeg.end());
  return cv::imdecode(buffer, cv::IMREAD_COLOR);
}
```

Borrar la implementación anterior completa y los `#include` de `libavformat`/`libavcodec`/`libswscale`/`libavutil` de `voice-test.cc` (`camera-audio.cc` los sigue necesitando hasta la Task 13). Eso elimina de paso el `memcpy` que copiaba `stride` bytes sobre filas de `width*3`.

- [ ] **Step 6: Vaciar el audio rancio al reanudar**

La causa 3 sobrevive al cambio de captura, porque el LLM y el TTS siguen bloqueando segundos. Tras **cada** etapa bloqueante — análisis de cámara, generación del LLM y reproducción por el altavoz — el audio acumulado no es un turno válido: se descarta y se reinicia el VAD.

```cpp
  const auto resumeListening = [&] {
    std::lock_guard<std::mutex> lock(bufMutex);
    camBuf.clear();
    vad.reset();
  };
```

Llamarlo justo antes de volver a poner `paused` a false, y también inmediatamente después de que `describeCamera()` devuelva. Sin el `vad.reset()` el estado LSTM de Silero cruza el hueco y arranca el turno siguiente contaminado.

- [ ] **Step 7: Medir la mejora**

```bash
./build/prod/labs/voice-test/argus-voice-test --cam-check
```

Cronometrar de la petición a la descripción, 3 veces, y anotar la mediana. Esperado: de 1-3 s (apertura RTSP) a la latencia del VLM sola (~1 s medido a `max_input_px=384`), porque go2rtc ya tiene el frame.

- [ ] **Step 8: Verificar el bug de extremo a extremo**

```bash
./build/prod/labs/voice-test/argus-voice-test --camera
```

Guion: saludar, pedir "analiza la cámara 1", esperar la descripción, y **seguir la conversación con dos frases más**. Repetir 3 veces. Esperado: las dos frases posteriores se transcriben correctamente y Argus no responde a nada que no se haya dicho después del análisis.

- [ ] **Step 9: Commit**

```bash
git add src/shared/services/stream labs/voice-test labs/stream-probe
git commit -m "fix(vision): grab camera frames from go2rtc and drop stale mic audio on resume"
```

## Fuera del alcance de este plan

- **`ConversationPipeline` en el backend.** Llevar el bucle de conversación de `labs/voice-test` a un servicio con su superficie WS (`voice_start`/`voice_stop`, audio binario entrante, `voice_state`) es un subsistema propio y merece su propio plan. Este plan deja listas las piezas que necesitará: `VadService`, `AudioResampler`, `CameraAudioSource` y la sesión de voz persistente.
- **AEC para barge-in durante la reproducción.** Interrumpir a Argus mientras habla exige cancelación de eco acústica real (WebRTC APM o similar). El control de eco de la Task 11 es mitigación por descarte, no AEC.
- **Fases C-G** (detección, tracking, memoria de personas, herramientas del LLM): siguen en `OPTIMIZATION_AND_MEMORY_PLAN.md`.
- **El stream type del muxer TS** (`0x06` vs `0x90`): necesita una comprobación contra la C225 física, no código. Anotado en `CONTEXT.md`.

## Orden de ejecución

El camino de voz es lo que está en uso hoy, así que va primero; el transporte de
vídeo por `/sync` todavía no tiene cliente y puede esperar sin bloquear a nadie.

```
Task 0   credenciales fuera del repo
Task 15  BUG REPORTADO: analizar la cámara sin segunda sesión RTSP
Task 6   AudioResampler + SampleRing + sonda de audio
Task 9   contexto de Silero y coste por ventana
Task 10  puerta de calidad de turno
Task 7   enchufar el resampler en el micrófono
Task 11  calibración con audio real y control de eco
Task 8   sesión de voz persistente
Task 4   AGC y guarda del micrófono
Task 5   código muerto y formato
Task 1   parser fMP4
Task 2   ciclo de vida del upstream
Task 3   créditos por conexión
Task 12  VadService a src/
Task 13  audio de cámara por go2rtc
Task 14  documentación
```

Dependencias duras: 7 y 9 necesitan 6; 10 necesita 9; 11 necesita 10 y 8; 12
necesita 10; 13 necesita 6 y 15; 2 y 3 necesitan 1; 14 va al final porque anota
números que producen las demás.
