# Plan — motor de extracción: elegir por medición, no por corazonada

## 1. Qué problema estamos resolviendo

La memoria se forma en dos tiers: un léxico determinista (µs) y un modelo
(~1.1 s, fuera del turno desde la Fase 7). El léxico cubre lo que conoce; el
modelo cubre el habla abierta. El objetivo del usuario es **precisión**, con
un presupuesto explícito de **1-2 s por captura** (corre en segundo plano) y
sin consumo desproporcionado.

### Fallos medidos del motor actual (NuExtract-1.5-tiny, Qwen2.5-0.5B, 469 MB)

| Frase | Salida cruda del modelo | Problema |
|---|---|---|
| `mi coche está en el garaje` | `action: "esté en el garaje"`, `object: ""` | Inventa el acento (no es span literal) y se come el objeto |
| — | — | Las invariantes lo rechazan → **el hecho no se guarda** |

Es decir: el modelo **acierta el sujeto** (`mi coche`) donde el léxico fallaba,
pero falla en literalidad. El patrón general: los modelos son mejores en
alcance/scoping, las reglas en literalidad.

### Lo que ya no es problema (no confundir capas)

- El sujeto `bicicleta` en vez de `bicicleta de ana` era **del léxico**, no del
  modelo. Corregido con una regla (sintagma completo), no con un map.
- La latencia del turno: la extracción ya no está en el camino del turno
  (0 ms medidos). Por eso 1-2 s de modelo son aceptables.

## 2. Lo que no puede regresionar

| Gate | Valor actual |
|---|---|
| `--holdout-test` recall | 20/20 (100 %) |
| `--holdout-test` precisión sujeto / acción-verbatim / slots distintos | 100 % / 100 % / 100 % |
| `--nuextract-test` verbatim | ≥ 90 % |
| `--extract-test` tier 1 solo | ≥ 40/60 |
| Turno hablado | 0 ms de extracción |
| RAM del proceso | modelo cargado solo en el worker, descargado a los 600 s |

Las **invariantes deterministas** (`TieredExtractor::addModelFacts`: acción y
valor tienen que ser spans literales, slots distintos, temporal resoluble,
complemento ≤ 6 palabras) **se quedan pase lo que pase**. Son el contrato que
hace sustituible al modelo: si mañana cambias de motor, la precisión no
depende de confiar en él.

## 3. Opciones

| # | Motor | Peso Q4_K_M | Español | Encaje |
|---|---|---|---|---|
| A | NuExtract-1.5-tiny (actual) | 469 MB | incidental | Ya integrado; techo de precisión bajo |
| B | NuExtract-1.5-smol (**ya descargado**) | 1.05 GB | incidental | Cambio de una línea de config; ~2-3× más lento |
| C | **LFM2-1.2B-Extract** | **697 MB** | **oficial** | Modelo *task-tuned* para texto→JSON, misma familia LFM2 que el chat |
| D | Reusar LFM2.5-1.2B-Instruct (chat) con GBNF | 0 MB extra | oficial | Modelo ya residente; segundo contexto ~pocos MB |
| E | Solo reglas, sin modelo | 0 | — | Descartado: el holdout demuestra que hace falta el modelo |

Notas por opción:

- **B** es la vía barata: `NuExtractService` ya soporta el formato v1.5, así
  que es literalmente cambiar `model_path`. Pero es la misma familia que ya
  falla en español y triplica la latencia.
- **C** es un modelo *entrenado para esta tarea* (documentos no estructurados →
  JSON/XML/YAML), con **español entre sus 9 idiomas oficiales**, basado en
  LFM2-1.2B: misma arquitectura híbrida que tu chat, o sea el mismo camino de
  llama.cpp y el mismo perfil de CPU que ya conoces. Recomienda greedy con
  `temperature = 0` y **system prompt con el esquema**, que es exactamente lo
  que hacemos con GBNF.
- **D** es la opción de coste cero en RAM y merece medirse aunque no gane: es
  el baseline honesto ("¿hace falta un modelo aparte?"). El riesgo es que
  compite por CPU con la conversación; se mitiga con un segundo contexto y el
  gate de `isBusy()` que ya usa el worker.

## 4. Recomendación

**Candidato principal: C (LFM2-1.2B-Extract), decidido por medición contra B y
D sobre la misma fixture.**

Razones, en orden de peso:

1. **Está entrenado para esto.** No es un instruct general al que le pedimos
   JSON: su tarea es extracción estructurada. Es la diferencia entre el 0.5B
   actual (que alucina acentos) y un modelo cuyo objetivo de entrenamiento es
   copiar spans a un esquema.
2. **Español oficial**, no incidental.
3. **Misma familia que el chat**: mismo runtime, misma plantilla ChatML, mismo
   comportamiento en CPU, misma licencia que ya tienes vendorizada.
4. **697 MB Q4_K_M** — más pequeño que el smol que ya descargaste (1.05 GB) y
   solo residente en el worker.
5. Cabe holgado: 30 GB de RAM, 16 núcleos.

**No recomiendo** saltar a un modelo grande "por si acaso": el cuello no es el
tamaño, es la afinidad a la tarea. El 0.5B actual falla en literalidad, no en
comprensión.

## 5. Cómo se decide (protocolo de medición)

Nada de cambiar el motor "porque la tarjeta del modelo lo dice". El veredicto
sale de correr los tres sobre la **misma** fixture con el **mismo** harness.

### 5.1 Ampliar la fixture antes de medir

La fixture actual no contiene los casos que nos han mordido. Añadir al
`--holdout-test` (siguen siendo verbos retenidos, no vistos por el léxico):

- **Scoping de poseedor**: `la bicicleta de ana está en el garaje`,
  `el gato de luis no come pescado` → el sujeto debe ser el sintagma completo.
- **Primera persona con objeto propio**: `mi coche está en el garaje` → sujeto
  `coche`, no `usuario`.
- **Acentos y tildes**: casos donde el modelo tiende a normalizar
  (`está`/`esté`) → mide literalidad.
- **Preguntas disfrazadas**: `¿a Pedro le gusta el pescado?` → no debe producir
  hecho (completitud).
- **Habla real con muletillas**: `mira, quiero que me recuerdas acerca de que…`

Gate nuevo: **scoping precision ≥ 90 %** (sujeto = sintagma completo cuando el
enunciado lo lleva).

### 5.2 Métricas por motor

| Métrica | Cómo |
|---|---|
| Recall | hechos producidos / enunciados |
| Precisión sujeto | sujeto correcto (incluye scoping) |
| Verbatim | acción y valor son spans literales del original |
| Slots distintos | acción ≠ valor ≠ tiempo |
| Rechazo correcto | preguntas y saludos no producen hecho |
| Latencia | p50 / p95 por extracción, en Release |
| RSS | pico con el modelo cargado |

### 5.3 Cómo se ejecuta

`labs/extract-probe` ya acepta `--model <ruta>` y `--format`. Falta:

1. Un tercer valor de `NuExtractFormat` (`Lfm`) para la plantilla ChatML con
   system prompt + esquema; el enum y `promptFor()` ya existen como seam.
2. `--engine-bench` que corra la fixture completa con N motores y saque la
   tabla comparativa de una vez, en vez de tres invocaciones manuales.

## 6. Fases

| Fase | Trabajo | Gate |
|---|---|---|
| 1 | Ampliar fixture con los casos de §5.1 (scoping, primera persona, tildes, preguntas, muletillas) | La fixture ampliada corre verde con el motor **actual** salvo en los casos que sabemos que falla — así queda documentado el punto de partida |
| 2 | Añadir `NuExtractFormat::Lfm` (ChatML + system prompt con esquema) y `--engine-bench` | Compila, 0 warnings; el motor actual sigue dando los mismos números |
| 3 | Descargar LFM2-1.2B-Extract Q4_K_M (697 MB) a `models/extract/` vía `scripts/setup.sh` | Checksum + arranque sin modelo sigue degradando al tier léxico |
| 4 | Correr `--engine-bench` con A, B, C y D | Tabla comparativa reproducible |
| 5 | **Veredicto** y cambio de `[extract] model_path` + `prompt_format` | Todos los gates de §2 en verde con el motor elegido |
| 6 | Decidir orden de tiers (§7) y ajustarlo | Turno sigue en 0 ms |

Fase 3 es la única que descarga algo; si el veredicto es "no compensa", se
borra el fichero y no queda deuda.

## 7. Decisión aparte: orden de los tiers

Hoy: léxico primero, modelo si el léxico no produce. Como la extracción ya
está **fuera del turno**, el argumento de latencia que justificaba ese orden se
ha debilitado.

- **Opción 1 (actual)**: léxico → modelo. Máxima velocidad, techo de precisión
  el del léxico cuando acierta *a medias* (p. ej. el sujeto `usuario` de antes).
- **Opción 2 (recomendada si C gana)**: modelo → léxico como respaldo cuando el
  modelo no está cargado o falla. Máxima precisión, ~1-2 s en segundo plano por
  captura, cero impacto en la conversación.
- **Opción 3**: léxico primero pero **escalar al modelo** cuando el resultado
  es sospechoso (sujeto de una sola palabra habiendo más contenido, sujeto
  `usuario` sin marca de primera persona en el sujeto). Ahorra el 80 % de las
  llamadas al modelo manteniendo su precisión.

Mi recomendación: **Opción 3** si C gana por poco, **Opción 2** si gana por
mucho. Se decide con la tabla, no antes.

## 8. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| El modelo nuevo no mejora lo suficiente | Se descarta y se queda A; el trabajo de fixture y `--engine-bench` se queda como activo permanente |
| Latencia por encima de 2 s | Se mide en Fase 4 con p95, no con medias; hay Q4_0 y 350M-Extract como escalones |
| RAM del worker | Solo carga en el worker y descarga a los 600 s (`idle_unload_seconds`), igual que hoy |
| Regresión silenciosa al cambiar de motor | Las invariantes deterministas y los gates de §2 no se tocan |
| Formato de prompt equivocado (ya nos pasó con NuExtract 2.0) | El formato es un valor del enum + fixture propia; se valida con `--extract-text` antes de correr el bench |

## 9. Resultado (cerrado)

Todas las fases ejecutadas. Veredicto: **se queda NuExtract-1.5-tiny**.

`argus-extract-probe --engine-bench` reproduce la comparativa en un comando:

| motor | recall | scoping | verbatim | distinct | ms |
|---|---|---|---|---|---|
| NuExtract-1.5-tiny | 28/28 | 86 % | 100 % | 100 % | 1041 |
| LFM2-1.2B-Extract | 26/28 | 96 % | 100 % | 100 % | 2987 |

El LFM gana en scoping y pierde en recall y latencia (2,9×). En voz real
además alimentaba de ruido al asistente. Se revirtió: `[extract] model_path`
y `prompt_format` vuelven a tiny, y `scripts/setup.sh` descarga tiny con
verificación de tamaño exacto y descarga reanudable.

**Lo que más movió la aguja no fue el motor sino la invariante**:
`trimSubject` llevó a tiny de 18 % a 86 % de scoping y al LFM de 0 % a 96 %.

### Orden de tiers: Opción 3 implementada

El léxico sigue primero, pero su resultado se acepta solo si **cubre** el
enunciado: si los slots extraídos suman menos de `extract.lexicon_min_coverage`
(0.6) de las palabras de la cláusula, la frase llevaba más de lo que el léxico
entendió y decide el modelo. Como la extracción ya corre fuera del turno, el
escalado no cuesta latencia de conversación.

Medido en voz: "mira una cosa, el sábado viene mi hermana, apúntalo" se guarda
**inline**; "a ver, escucha, es que el router ese lo tengo en el trastero,
apúntalo" se **difiere** al modelo. Ambos hechos quedan correctos.
