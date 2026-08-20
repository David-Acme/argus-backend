# Argus — Plan detallado para reducir la dependencia del tool-calling

**Destinatario:** Claude, agente de implementación  
**Proyecto:** Argus backend  
**Ruta:** `/home/acme/Desktop/argus/backend`  
**Fecha:** 2026-08-20  
**Estado:** plan de trabajo; validar el árbol actual antes de continuar

## 1. Objetivo

Mejorar Argus para que el modelo conversacional no necesite emitir tool calls
para responder, guardar memoria, recuperar memoria o activar acciones simples.
La infraestructura de tools no debe eliminarse completamente: debe quedar
disponible para pruebas, administración y acciones explícitas que realmente
necesiten un contrato estructurado.

El diseño final debe funcionar con modelos pequeños, sin capacidad nativa de
tool-calling y en computadoras con pocos recursos. La evaluación debe medir no
solo flexibilidad, sino también precisión, velocidad, RAM, facilidad de
depuración y comportamiento cuando el modelo cambia.

La propuesta inicial es una hipótesis, no una decisión irreversible:

    reglas deterministas
      + fastText como hint barato
      + extracción en segundo plano
      + recall determinista
      + una generación normal del LLM
      + tools solamente en rutas explícitas

Claude debe comparar esta propuesta con reglas solamente, un router LLM
separado, un extractor estructurado dedicado y tool-calling del LLM principal.

## 2. Instrucciones obligatorias para Claude

### 2.1 Leer instrucciones antes de generar código

Antes de crear o editar cualquier archivo, Claude debe leer completamente:

1. `/home/acme/Desktop/argus/backend/AGENTS.md`.
2. `CONTEXT.md`.
3. Este archivo, `CLAUDE_PLAN.md`.
4. Los `AGENTS.md` y `CONTEXT.md` del frontend si un cambio afecta contratos
   HTTP, WebSocket, DTO, permisos o sincronización.

No basta con leer el bloque de instrucciones recibido en el prompt: debe
confirmar el contenido del archivo local antes de modificar código.

### 2.2 No llenar el código de comentarios

No añadir comentarios largos, narrativos o que repitan lo que el código ya
expresa. Preferir nombres claros, funciones pequeñas y estructuras explícitas.

Un comentario nuevo solo está justificado si documenta una invariancia no
obvia, una limitación de una biblioteca externa, una condición de concurrencia
o una decisión de seguridad. Debe ser corto, específico y cercano a la línea
relevante.

No convertir cada cambio en un bloque de explicación dentro del `.cc` o `.hxx`.
Las explicaciones extensas van en `CONTEXT.md` o en este plan.

### 2.3 Proteger cambios existentes

El árbol puede contener modificaciones legítimas del usuario. Antes de tocarlo:

    git status --short
    git diff --stat

No ejecutar:

- `git reset --hard`;
- `git checkout --`;
- borrados recursivos amplios;
- limpieza de modelos, bases de datos o logs sin autorización;
- reformateo masivo de archivos no relacionados.

Usar `apply_patch`, revisar el diff después de cada grupo de cambios y separar
los cambios de este plan de los cambios preexistentes.

### 2.4 Cuidar los recursos de la computadora

Antes de una compilación completa, una prueba de voz o una inferencia:

    pgrep -af 'ollama|argus-llm-bench|voice-test|llama|cmake --build' || true

Si hay otro trabajo pesado activo, no iniciar otra carga sin avisar. Las
búsquedas, inspecciones y pruebas pequeñas sí pueden ejecutarse.

## 3. Estado conocido del proyecto

### 3.1 Modelo de referencia

El modelo actual de referencia es LFM2.5 1.2B Instruct. En la última batería
Ollama obtuvo aproximadamente:

| Caso | tok/s |
|---|---:|
| corto | 48.17 |
| medio | 46.22 |
| largo | 43.40 |
| muy largo | 40.09 |
| extremo | 35.08 |
| batería de memoria | 48.45 |

La batería de memoria obtuvo recall 8/8, atribución 8/8, no invención 2/2 y
0/10 respuestas con pensamiento visible.

Resultados recientes de alternativas:

| Modelo | Velocidad de memoria | Recall | Atribución | No invención |
|---|---:|---:|---:|---:|
| LFM2.5 1.2B | 48.45 tok/s | 8/8 | 8/8 | 2/2 |
| Granite 3.3 2B | 21.61 tok/s | 8/8 | 8/8 | 2/2 |
| Granite 4.0 H-1B | 31.32 tok/s | 6/8 | 8/8 | 2/2 |
| Granite 4.0 1B | 33.83 tok/s | 7/8 | 8/8 | 2/2 |
| Granite 4.0 Micro 3B | 17.79 tok/s | 6/8 | 8/8 | 2/2 |

Granite H-Micro 3B fue descargado, pero no fue evaluado. No reabrir la búsqueda
de modelos en este plan salvo que el usuario lo solicite explícitamente.

### 3.2 Rutas de ejecución

`labs/voice-test/voice-test.cc` es el laboratorio principal de voz, memoria,
cámara, STT, LLM y TTS. Su flujo actual es:

    entrada
      -> IntentService opcional
      -> cámara opcional
      -> MemoryService::captureExplicit
      -> MemoryService::captureImplicit si fastText lo autoriza
      -> GraphRecall
      -> LlmService::chatStream
      -> TTS en los modos de voz
      -> historial y compaction

`src/feature/socket/sync/services/voice-session-service.cc` es la ruta real del
WebSocket de voz. Actualmente usa VAD, STT, `voiceLlm()` y TTS, pero no utiliza
`MemoryService` ni `ConversationService`. No asumir que un cambio del lab ya
modificó el backend productivo.

`src/shared/services/conversation/` contiene la abstracción general de
conversación. Originalmente enumeraba `ToolRegistry`, construía descriptores y
llamaba `LfmAdapter::chatWithTools()`. El camino normal debe pasar a una sola
generación de `LlmService::chat()` o `chatStream()`.

### 3.3 Memoria

`MemoryService` coordina `SqliteGraph`, `MemoryFormation`, `RuleParser`,
`TieredExtractor`, `EmbeddingService`, `GraphRecall`, el worker y la
compaction. Las fronteras relevantes son:

- `captureExplicit`: trigger o statement reconocido por reglas;
- `captureImplicit`: candidato aprobado por un caller ligero;
- `captureToolCall`: compatibilidad con tools explícitos;
- `enqueueSummary` y `enqueueCompaction`: episodios en segundo plano.

`RuleParser` ya reconoce triggers, statements, interrogativos, recall markers,
fillers y confirmaciones en el vocabulario estático español/inglés.

### 3.4 Intent y datos

`IntentService` usa fastText para `camera` y `memory_save`. El corpus tiene
aproximadamente 1406 ejemplos de cámara, 351 de memoria y 5161 de `none`.

`labs/intent-data/usage.tsv` es telemetría histórica. No es ground truth. Ya
contiene falsos positivos como preguntas, recalls y cancelaciones etiquetados
como `memory_save`. Nunca volver a mezclarlo automáticamente con `train.tsv`.

### 3.5 Tools existentes

`ToolRegistry`, `ToolExecutor`, `LfmAdapter`, `ToolParser` y
`MemoryService::registerTools()` deben conservarse para `labs/tool-bench`,
`labs/memory-probe`, automatizaciones explícitas y pruebas de compatibilidad.
Que el diálogo normal no use tools no justifica borrar esos componentes.

## 4. Causa de los falsos guardados

El problema de una entrada como “Argus, cuándo es 2 x 2” tiene varias capas:

1. fastText puede disparar `memory_save` por una clasificación imperfecta.
2. `captureImplicit()` históricamente encolaba cualquier texto recibido.
3. `voice-test` imprimía “queued by intent” aunque la formación posterior
   pudiera rechazar el texto.
4. `usage.tsv` registraba predicciones y luego podía usarse para entrenar como
   si fueran etiquetas manuales.
5. `enqueueSummary()` podía recibir preguntas y confiar solamente en que otro
   LLM las omitiera.

La corrección debe separar:

    predicción -> política -> persistencia -> telemetría -> entrenamiento

Una predicción no es una política y una política aceptada no es automáticamente
un ejemplo de entrenamiento perfecto.

## 5. Comparación de alternativas

### 5.1 Solo reglas

Flujo:

    RuleParser -> guardar o rechazar -> LLM normal

Es la opción más rápida y explicable. Tiene alta precisión en “recuerda que”,
“anota que” y frases cubiertas por el vocabulario, pero menor recall para
declaraciones naturales no previstas.

Debe existir siempre como fallback cuando fastText o un extractor no estén
disponibles.

### 5.2 Reglas + fastText + extractor idle

Flujo:

    RuleParser pregunta
      -> fastText como hint
      -> MemoryService decide
      -> extractor lexicon/model en worker
      -> LLM conversacional sin tools

Es la opción recomendada provisionalmente porque conserva baja latencia y
permite declaraciones implícitas. Requiere un corpus limpio, una barrera
determinista contra preguntas y una cola que no bloquee la voz.

### 5.3 Router LLM pequeño

Un modelo separado puede clasificar `conversation`, `memory_save`, `camera` o
`recall`. Puede mejorar casos ambiguos, pero introduce otra inferencia por
turno, otra dependencia de RAM/CPU y otra fuente de errores. Solo probarlo si
fastText + reglas no alcanza una precisión aceptable con datos reales.

### 5.4 Tool-calling del LLM principal

Es flexible para acciones complejas, pero añade descriptores, tokens, parsing,
variabilidad y posiblemente varias generaciones. No debe ser requisito del
diálogo normal. Mantenerlo aislado para rutas que realmente lo necesitan.

### 5.5 Extractor dedicado fuera de turno

Permite separar formación de memoria y conversación. Puede ser muy preciso,
pero aumenta consumo y hace que la memoria aparezca con retraso. Debe ser
opcional, cancelable y de prioridad inferior a la respuesta de voz.

### 5.6 Decisión provisional

Implementar primero la alternativa 5.2, medirla y compararla contra 5.1. No
añadir un router LLM hasta demostrar que fastText y las reglas no son
suficientes.

## 6. Arquitectura objetivo

### 6.1 Flujo normal

    STT o texto
      -> normalización e idioma
      -> captureExplicit
      -> isQuestion para la ruta implícita
      -> fastText como hint
      -> cola de formación si es candidato válido
      -> recall determinista
      -> una generación normal del LLM
      -> TTS o respuesta HTTP/WS

### 6.2 Invariantes

1. Una pregunta no se guarda como memoria implícita.
2. Un intent no escribe directamente en SQLite.
3. `Rejected` significa que no se encoló ni persistió nada.
4. `Deferred` significa que existe trabajo válido en la cola.
5. `Stored` significa que existe un fact persistido.
6. El LLM conversacional no recibe descriptores de tools en la ruta normal.
7. El LLM no debe producir JSON, tags ni bloques para guardar memoria.
8. Solo se confirma una memoria si el backend confirma la captura.
9. La extracción en background no debe bloquear la primera respuesta de voz.
10. Las acciones sensibles validan permisos fuera del LLM.
11. `ToolRegistry` mantiene sus validaciones de `role_access`.
12. Los probes pueden seguir ejercitando tool-calling aislado.

## 7. Cambios parciales presentes en el árbol

Hay un parche parcial que Claude debe revisar y compilar antes de ampliarlo.

### 7.1 `MemoryService::captureImplicit`

Se añadió una comprobación de usuario, texto vacío y `RuleParser::isQuestion()`
antes de encolar. Verificar que:

- rechace “¿cuánto es 2 x 2?”;
- rechace “Argus, cuando es 2 x 2” sin signo de pregunta;
- rechace “¿cuándo viene mi hermana?”;
- permita que `captureExplicit()` maneje triggers explícitos;
- no rompa statements implícitos legítimos;
- no reporte `Deferred` si la entrada fue rechazada.

### 7.2 `voice-test`

Se retiró el procesamiento activo de `ToolParser` en los loops de texto,
cámara y voz. Los tokens se manejan como respuesta normal. `ToolParser` debe
seguir compilando para los probes que lo necesiten.

El log de `memory_save` debe ocurrir después de que `captureImplicit()` acepte,
no inmediatamente después de la predicción de fastText.

### 7.3 `ConversationService`

Se cambió el camino normal a captura explícita, recall, perfil y una llamada
normal a `LlmService`. Se introdujo `ConversationTurnInput` para no mantener
parámetros de tool-calling que ya no son necesarios en esa ruta.

Claude debe buscar todos los call sites antes de cambiar nuevamente la firma.

## 8. Fases de implementación

### Fase 0 — Baseline

1. Leer `AGENTS.md`, `CONTEXT.md` y este plan.
2. Revisar estado y diff.
3. Buscar call sites de `processTurn()` y `chatWithTools()`.
4. Confirmar que no haya inferencias activas.
5. Compilar o hacer una comprobación focalizada para detectar errores del
   parche parcial.

Salida: mapa de rutas y lista de errores sin mezclar cambios no relacionados.

### Fase 1 — Contrato de captura

1. Mantener `captureExplicit()` como primera ruta.
2. Rechazar en `captureImplicit()` usuario inválido, texto vacío, preguntas,
   recall markers, cancelaciones y olvidos.
3. Mantener la formación como segunda validación, nunca como único filtro.
4. Ajustar mensajes de `voice-test` para que reflejen el `CaptureOutcome` real.
5. Añadir pruebas para español e inglés.

Casos negativos mínimos:

    ¿cuánto es 2 x 2?
    Argus, cuando es 2 x 2
    ¿cuándo viene mi hermana?
    ¿qué me gusta tomar?
    qué no le gusta a Rodrigo
    no, olvídalo

Casos positivos mínimos:

    recuerda que mi hermana viene los domingos
    anota que llegó el paquete
    ten en cuenta que soy alérgico a los frutos secos
    mi perro se llama Toby

### Fase 2 — Compaction segura

1. Filtrar preguntas de las líneas `user:` antes de `enqueueSummary()` y
   `enqueueCompaction()`.
2. Eliminar la respuesta `assistant:` inmediatamente asociada cuando el turno
   completo sea una consulta transitoria.
3. No encolar si no queda contenido útil.
4. Mantener el prompt de compaction como defensa secundaria.
5. Probar que una sesión solo de saludos, preguntas y cálculos no crea un
   episodio.
6. Probar que una declaración durable sí aparece en el resumen cuando procede.

### Fase 3 — Datos de fastText

1. Mantener `usage.tsv` como telemetría.
2. No fusionarlo automáticamente con `train.tsv`.
3. Crear un flujo de datos curado, por ejemplo `usage-curated.tsv`, o un flag
   que exija revisión explícita.
4. Si se conservan muestras históricas, descartar preguntas, recalls,
   cancelaciones y frases ambiguas.
5. Mostrar conteos de líneas aceptadas y rechazadas.
6. Añadir casos negativos de cálculo, hora, fecha, clima, recall y small talk.
7. Medir precision, recall y falsos positivos sobre preguntas.
8. No bajar el threshold solo para hacer pasar un corpus pequeño.

La precisión debe pesar más que el recall para `memory_save`: un falso positivo
contamina hechos futuros y un falso negativo puede corregirse con un trigger
explícito.

### Fase 4 — Prompt sin tools

Añadir al prompt, en español e inglés, instrucciones breves equivalentes a:

    Las preguntas, cálculos, fechas, horas, definiciones, chistes y consultas
    son conversación efímera; respóndelos y no los trates como memoria.

    La memoria se administra fuera del modelo. No emitas tool calls, JSON,
    etiquetas especiales ni bloques de guardado.

    Solo confirma que algo fue guardado si el sistema añadió una nota de captura
    aceptada.

No llenar el prompt con instrucciones repetidas. Medir el tamaño del prefijo y
conservar la reutilización de KV cache.

### Fase 5 — ConversationService

El orden debe ser:

    validar texto
    captureExplicit
    recallBlock
    profileFor
    construir mensaje del usuario
    LlmService::chat o chatStream
    actualizar historial
    trimHistory

No debe enumerar tools, construir `ToolDescriptor`, llamar `chatWithTools`,
ejecutar `ToolExecutor` por decisión del modelo ni usar `maxToolHops`.

Los tools externos siguen disponibles para probes y rutas explícitas.

### Fase 6 — Prioridad del worker

Auditar `deferCapture()`, `enqueueJob()` y `processExtract()`. La captura
implícita que requiera extractor LLM debe poder marcarse como trabajo idle.

Objetivo:

- responder primero;
- formar memoria después;
- reencolar si `LlmService::isBusy()`;
- evitar que el extractor se convierta en una segunda generación bloqueante;
- conservar la memoria explícita determinista en la ruta de mayor prioridad.

No cambiar la política sin medir la latencia y revisar la concurrencia del
contexto compartido de `LlmService`.

### Fase 7 — VoiceSessionService productivo

No copiar globals del lab. Antes de integrar:

1. determinar cómo obtener `MemoryService` por inyección;
2. agregar `WorkingMemory` por sesión si corresponde;
3. usar el `userId` autenticado;
4. resolver idioma por sesión;
5. definir la notificación de captura aceptada;
6. definir cancelación al cerrar WebSocket;
7. asegurar que STT, LLM, TTS y memoria respeten sus mutex/slots;
8. añadir pruebas específicas de WebSocket.

## 9. Prompt y comportamiento esperado

Para la entrada “Argus, cuando es 2 x 2” el resultado correcto es:

1. `captureExplicit()` no encuentra una memoria explícita.
2. `captureImplicit()` rechaza porque es una pregunta.
3. No se encola extracción.
4. No se escribe fact ni episodio.
5. El LLM responde la pregunta normalmente.
6. TTS solo recibe la respuesta hablada.

Para “recuerda que mi hermana viene los domingos”:

1. `captureExplicit()` guarda o encola una captura válida.
2. El mensaje contiene una nota de captura únicamente si el resultado lo
   permite.
3. El LLM confirma brevemente sin generar un tool call.
4. El recall posterior recupera el hecho.

Para “mi perro se llama Toby”:

1. fastText puede actuar como hint si el modelo está disponible.
2. La pregunta determinista no dispara.
3. `MemoryService` decide si encola extracción.
4. La respuesta del usuario no espera otro ciclo de tool-calling.

Para “no, olvídalo”:

1. no se guarda como fact;
2. no se usa como muestra positiva automática;
3. si existe una operación explícita de olvidar, debe tener una ruta separada
   y validada.

## 10. Pruebas

### 10.1 Búsquedas estáticas

Ejecutar:

    rg -n "chatWithTools|ToolParser|captureToolCall" \
      src/shared/services/conversation labs/voice-test

Esperado:

- no `chatWithTools` en el camino normal;
- no `ToolParser` en los loops activos de `voice-test`;
- `captureToolCall` solo en compatibilidad, probes o tests.

### 10.2 Compilación

Ejecutar primero el target focalizado si existe. Luego:

    cmake --preset dev
    cmake --build --preset dev -j 8

Requisito: 0 errores y 0 warnings nuevos. No continuar con pruebas pesadas si
la compilación falla.

### 10.3 Intent

Ejecutar el `intent-probe` existente y revisar especialmente los casos con:

- `2 x 2`;
- `cuándo viene`;
- `qué me gusta`;
- `qué no le gusta`;
- `olvídalo`;
- `what time`;
- `tell me`.

Ninguno debe disparar `memory_save` al threshold de producción.

### 10.4 Memoria

Probar:

1. declaración explícita almacenada;
2. pregunta rechazada antes de cola;
3. declaración implícita aprobada;
4. recall posterior;
5. cancelación rechazada;
6. compaction sin preguntas como episodio;
7. compaction con declaración durable;
8. no invención cuando no existe recall.

### 10.5 Voz

Solo ejecutar `voice-test` cuando el usuario lo autorice y la computadora no
esté ocupada. Verificar que no se pronuncien tags, JSON ni texto interno.

### 10.6 Métricas

Comparar en condiciones equivalentes:

| Métrica | Antes | Después |
|---|---:|---:|
| TTFT sin memoria | medir | medir |
| TTFT con recall | medir | medir |
| generaciones por turno | medir | objetivo 1 |
| tokens de schema de tools | medir | objetivo 0 |
| falsos `memory_save` en preguntas | medir | objetivo 0 |
| disponibilidad de memoria implícita | medir | medir |

No comparar una máquina ocupada contra una libre y atribuir la diferencia al
modelo.

## 11. Documentación en `CONTEXT.md`

Añadir una sección fechada con:

1. tabla LFM2.5 vs Granite;
2. aclaración de que H-Micro fue descargado, no evaluado;
3. decisión de no exigir tool-calling al diálogo normal;
4. separación entre `RuleParser`, `IntentService`, `MemoryService`,
   `GraphRecall`, `LlmService` y `ToolRegistry`;
5. problema de `usage.tsv` como telemetría no curada;
6. diferencia entre `labs/voice-test` y `VoiceSessionService` productivo;
7. métricas antes/después;
8. cualquier limitación pendiente.

## 12. Riesgos

### Pérdida de declaraciones implícitas

Mitigar con vocabulario, ejemplos reales, fastText como hint y un trigger
explícito como fallback.

### Bloqueo de voz por extractor

Marcar trabajo implícito como idle, reencolar si el LLM está ocupado y medir
latencia de primera respuesta.

### Salida del modelo con tags

No incluir schemas, reforzar el prompt y usar sanitizer solo para ocultar
artefactos; nunca interpretar ese sanitizer como ejecución de una acción.

### Cambio de firma pública

Buscar call sites, adaptar consumidores reales y compilar inmediatamente.

### Confundir episode con fact

Probar tablas y rutas de recall separadamente. No llamar “guardado” a un
summary provisional.

### Contaminación del corpus

Separar telemetría, muestras curadas y corpus de entrenamiento. No usar
predicciones como etiquetas sin revisión.

## 13. Criterios de aceptación

- [ ] Claude leyó `AGENTS.md` local antes de modificar código.
- [ ] No se añadieron bloques de comentarios largos al código.
- [ ] El diálogo normal no llama `LfmAdapter::chatWithTools()`.
- [ ] El diálogo normal no anuncia tools al LLM.
- [ ] `ToolRegistry` sigue funcionando en probes y rutas explícitas.
- [ ] Las preguntas no entran a `captureImplicit()`.
- [ ] “Argus, cuándo es 2 x 2” no aparece como fact ni episodio.
- [ ] Las preguntas de recall no disparan `memory_save`.
- [ ] “olvídalo” no se guarda.
- [ ] `usage.tsv` no recibe predicciones crudas como etiquetas de entrenamiento.
- [ ] Compaction filtra preguntas.
- [ ] El prompt indica que memoria se administra fuera del modelo.
- [ ] La ruta normal usa una generación.
- [ ] TTS no pronuncia protocolos internos.
- [ ] Se midieron falsos positivos y latencia.
- [ ] `CONTEXT.md` documenta la decisión.
- [ ] El build dev termina con 0 errores y 0 warnings.

## 14. Orden de ejecución

    1. Leer AGENTS.md, CONTEXT.md y CLAUDE_PLAN.md.
    2. Revisar status y diff.
    3. Confirmar procesos pesados.
    4. Buscar call sites y reparar errores del parche parcial.
    5. Terminar contrato de captureImplicit.
    6. Terminar filtro de compaction.
    7. Separar usage.tsv del corpus curado.
    8. Añadir casos negativos y positivos revisados.
    9. Actualizar prompts sin tool-calling.
    10. Compilar dev.
    11. Ejecutar pruebas focalizadas.
    12. Ejecutar voz solo con autorización.
    13. Medir latencia y precisión.
    14. Documentar en CONTEXT.md.
    15. Revisar diff final y entregar resultados.

## 15. Entrega de Claude

El resumen final debe incluir:

1. archivos modificados;
2. archivos deliberadamente no modificados;
3. alternativa elegida y alternativas descartadas;
4. pruebas ejecutadas y resultados;
5. falsos positivos y recall de memoria;
6. latencia antes/después;
7. limitaciones pendientes;
8. pruebas pesadas que no se ejecutaron y por qué;
9. confirmación de que no se borraron modelos, bases de datos ni cambios del
   usuario;
10. confirmación de que el diálogo normal no depende del tool-calling.

No declarar el trabajo terminado solo porque compile. Debe demostrarse el
comportamiento con preguntas, cálculos, declaraciones explícitas,
declaraciones implícitas, recalls, cancelaciones y compaction.
