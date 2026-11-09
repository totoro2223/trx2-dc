# Plan: refactor de código y datos para bajar la RAM

**Estado (09/11/2026):** la rama `refactorizacion-masiva` ya está en `dreamcast`. Hechos A1 (sin uso sin Lua), A2, A3, A4, A5, C1/C2, C4, D4 y D5 (por PLAN_SIN_LUA); pasaron los recorridos de niveles sin Lua (TR1 16, TR2 18, TR3 20) y P2 de TR2 (PROBADO, `brk` mínimo 2684 K), sin medida propia de cada cambio. A medias: B (caché de transparencia en memoria; páginas por hash y `levelpages.bin` del 09/11 sin commit; `.DCL` sin empezar), D2 (`SECTOR` a 48 B; decodificar al consultar descartado), D3 (`MAX_ITEMS` 512 fijo), D7 (solo el catálogo). Sin hacer: C3, D1, D6.

Preparado el 02/11/2026. Objetivo: que la consola deje de quedarse sin memoria
en partida, con margen (`brk` ≥ 1 MB del final en el peor nivel, hoy 76–124 K
en Barkhang con guardados). Antes de tocar nada lee
[REPORTE_MEMORIA.md](REPORTE_MEMORIA.md) y
[PLAN_MEMORIA_SESIONES_LARGAS.md](PLAN_MEMORIA_SESIONES_LARGAS.md): este plan
no repite lo que ya está hecho (región del nivel, arena del guardado, BSON en
trozos, desalojo de mallas).

## 1. ¿Sirve algo del fork de OpenLara para Dreamcast?

Revisado `histat/OpenLara-dc` (último commit 23/11/2024, copia en
`D:\trx\_downloads\OpenLara-dc`, fuera de git).

| Pieza | Qué hace | ¿Sirve para la RAM? |
|---|---|---|
| Núcleo (`level.h`, `format.h`) | OpenLara sin cambios: lee el nivel **entero** a RAM y no libera nada hasta el siguiente. Solo carga TR1 de PC (`ASSERT VER_TR1_PC` bajo `_GAPI_TA`). | **No.** No hay streaming ni limpieza; es el mismo modelo que tenemos, con menos motor encima (sin Lua). |
| `gapi/ta.h` | Texturas de TR1 en 8 bpp con paleta (`PC_PALETTE_8B`) en VRAM; mallas copiadas a `new[]` en RAM. | No: ahorra VRAM, no RAM, y nuestro VQ ya ahorra más. Sus mallas duplican datos en RAM (peor que lo nuestro). |
| `aica/` + `audio.c` | Firmware propio del ARM con sonidos registrados y anillo estéreo. | No: los efectos ya van directos a la RAM de sonido (ADPCM, `Audio_Sample_Load`). Solo interesaría si un día se mezcla en el ARM. |
| `sh4_math.h`, `xmtrx.S`, `sq.h`, `pvr_cxt.h` | Matemática SH-4, envío directo al TA, store queues. | No para RAM; ya tenemos equivalentes (`xform_*_sh4.S`, tarea 71). |
| `main.cpp` (VMU) | `vmufs_read/write` del fichero entero. | No: nuestro `vmu.c` ya hace más. |

Conclusión: **nada aprovechable para la memoria**. Lo que nos separa de
OpenLara en RAM no es el cargador, es lo que TRX añade (Lua, catálogos,
inyecciones en tiempo de carga). Eso es lo que ataca este plan.

## 2. Dónde está la RAM hoy (TR2 Barkhang, el peor caso)

| Bloque | Tamaño | Fuente |
|---|---:|---|
| `.text` | 2 215 K | `sh-elf-size` del ELF de `dc-minsize` (30/10) |
| `.rodata` + `.data` | ~470 K | ídem |
| `.bss` | 729 K | ídem (tiempo, chispas, luces de ítem, colas: ~50 K cada uno) |
| Región de Lua (fija) | 2 304 K | `dc_lua_alloc.c`: 1280 K pequeños + 1024 K grandes; **en partida usa 1,2–1,4 MB** |
| Región del nivel | 6 591 K | pico 95 % en carga, 100 % con guardados |
| Bloques pequeños de newlib | ~2 200 K | dueño desconocido (M3 del plan anterior) |
| **Total** | **~14,5 MB** de 16 | |

El nivel cargado ocupa 2,5–3,6 MB; el resto de la región es **pico de carga**
(inyecciones infladas, conversión de texturas, datos crudos de sala).

## 3. Streaming: qué sí y qué no

- **En partida, geometría/animaciones: no.** La lógica de TR necesita todas las
  salas (suelo, cajas de IA, salas alternas), todas las animaciones de los
  enemigos del nivel y los ítems siempre. Hacer streaming de eso es reescribir
  el motor, para ganar poco: el nivel residente son 2,5–3,6 MB.
  Lo único "streameable" en partida ya se hace: música (CD), efectos (AICA),
  texturas (VRAM) y mallas de objeto no dibujadas (desalojo).
- **En carga: sí, y es lo que más rinde.** El pico de carga dobla lo residente.
  Hornear los niveles en el PC (fase B) convierte la carga en "leer del disco
  directo a la estructura final", sin inflar ni convertir: pico ≈ residente.
- **Limpieza en partida: sí, en puntos seguros** (fase C): GC de Lua completo,
  devolver la cima de newlib, y quitar las reservas por frame.

## 4. Fases (por ganancia/riesgo)

Cada fase: medir antes/después con `mem_summary.py` en P1/P2 de TR2 y TR3, y
anotar en PROBADO.md. Lo que no gane, se deshace.

### Fase A — ganancias directas, riesgo bajo (~1,0–1,3 MB)

| # | Cambio | Ganancia estimada | Dónde |
|---|---|---:|---|
| A1 | **Región de Lua ajustada**: grandes de 1024 K → 512 K; el diálogo de Graphics (pico ~950 K) se desborda a la región del nivel, que en partida tiene ~3 MB libres (hoy desborda a newlib). | ~500 K | `dc_lua_alloc.c` (`M_BIG_SIZE`, camino de `spilled`) |
| A2 | **`.bss` a demanda**: `m_Raindrops`/`m_Snowflakes` (98 K, solo niveles con tiempo), `m_ItemLights` (48 K), `m_BumpQueue` (45 K, solo con relieve), `m_Sparks` (43 K, solo TR3), `m_Bench` (24 K, solo bench): reservarlos en la región del nivel al cargar y solo si se usan. | ~200–250 K | `fx/weather.c`, `render/` de DC (bajo `__DREAMCAST__`) |
| A3 | **`-O2` solo en lo caliente** (render, xform, colisión, Lara), `-Os` en el resto (`.text` era 2,69 MB en `-Os` total frente a 2,91 en `-O2`). | ~150–200 K | `src/platform/dreamcast/meson.build`, `build.sh` |
| A4 | **Quitar `pcre2` en DC** (85 K de código): 6 llamadas a `String_Match`, sustituibles por comparación simple o `fnmatch`. | ~90 K + su heap | `core/strings/common.c` bajo `__DREAMCAST__` |
| A5 | **Censo de los 2,2 MB de newlib** (M3): build con `DC_ALLOC_TRACE_MIN` 1–4 K + `live_census.py`. Sin esto no se sabe qué recortar; candidatos: catálogo de cadenas, `game_flow`, config, `m_CatalogEntryDefs` duplicados en heap. | se decide tras medir | `dc_alloc_trace.c` |

### Fase B — niveles horneados en el disco (refactor de datos, pico −1,5…−3 MB)

Una herramienta de PC (junto a `texvq.c`, en `tools/dreamcast/`) que por cada
nivel escribe un `.DCL` ya listo para la consola:

1. **Inyecciones ya aplicadas**: hoy cada carga lee 4,3–5 MB comprimidos de
   inyecciones y las infla junto al nivel (`Inject_AllInjections`,
   `level/pipeline.c:235`). Horneadas, en DC no se lee ninguna.
2. **Sin texturas** (ya están en `textures.vq`/`.bump`) ni sonidos (en
   `main.sfx`).
3. **Vértices de sala ya compartidos** (`M_ShareVertices`) y sin los datos
   crudos de sala (~1,7 MB que hoy guarda `shadows.c`; TAREAS 15: precalcular
   en el horno lo que `shadows.c` lee de ellos).
4. **Caché de transparencia de objetos** dentro del fichero (TAREAS 66):
   desaparece el redibujado de todas las mallas en cada carga.
5. **Formato plano con offsets**: cada sección se lee con `fread` directo a su
   bloque final de la región, en el orden en que se queda (lo de larga vida
   abajo), sin búferes intermedios.

El cargador de DC elige `.DCL` si existe y cae al camino actual si no (para no
romper TR1 clásico ni expansiones mientras se hornean). Riesgo: medio — el
horno debe reproducir exactamente lo que hacen las inyecciones; se valida
comparando un volcado de las estructuras cargadas por los dos caminos en el
host o en Flycast (mismo hash por sección).

Ganancia: pico de carga ≈ residente (2,5–3,6 MB en vez de 5,4–6,6 MB), carga
más rápida en GDEMU (menos bytes y sin inflar), y ~1,7 MB menos residentes si
se quitan los datos crudos de sala. Con eso la región del nivel puede
**encogerse** (~5 MB) y lo que sobra pasa a margen de newlib.

### Fase C — limpieza en partida (estabilidad en sesiones largas)

| # | Cambio | Dónde |
|---|---|---|
| C1 | `lua_gc(L, LUA_GCCOLLECT)` en puntos seguros: fin de carga, cerrar pasaporte/menú, tras guardar. | `lua/` de DC |
| C2 | `malloc_trim(0)` de newlib tras cada carga y al salir de menús (devuelve la cima libre). | `dc_level_heap.c` |
| C3 | Reservas por frame (~30: `Catalog_CreateKey`, `Memory_DupStr`, `String_Format`) a una arena de frame que se vacía cada frame (TAREAS 9 / M11). | `core/memory.c` bajo `__DREAMCAST__` |
| C4 | Mapa de solapes de salas liberado en `Level_Unload` (TAREAS 69). | `rooms/common.c` |

### Fase D — cambios drásticos en datos y motor (TR1, TR2 y TR3)

**Medida (02/11/2026)**: lo que pesa un nivel **en el fichero, sin texturas
ni sonidos** (script `tr2sections.py` del scratchpad, a pasar a
`tools/dreamcast/level_sizes.py` si se usa más):

| Juego | Niveles medidos | Datos del nivel | Mayores secciones |
|---|---|---:|---|
| TR1 | Caves … Atlantis (10C) | 595–758 K | fotogramas 271–412 K, malla de salas 108–177 K, mallas de objeto 66–108 K |
| TR2 | Great Wall … Barkhang, Emperor's Tomb | 803–1150 K | fotogramas 351–423 K, salas 150–361 K, objetos 116–145 K |
| TR3 | Jungle, Temple, City, Mines, Roofs, Chamber | 811–1188 K | fotogramas 362–425 K, salas 85–368 K, objetos 144–181 K |

En RAM ese mismo nivel ocupa **2,5–3,6 MB**: entre **3 y 4 veces** lo que
pesa en el fichero. El problema no son los datos, es cómo se inflan:

| Dato | Fichero | En RAM (DC hoy) | Por qué |
|---|---:|---:|---|
| Sector | 8 B | 56 B (`SECTOR` + 2 `SURFACE`, `rooms/types.h`) | el suelo/techo se decodifica entero al cargar, con punteros a disparadores y walkables |
| Vértice de sala | 8 B (TR1) / 12 B | 16 B (`ROOM_VERTEX`) **+** ~28 B por esquina (`OUTPUT_MESH_VERTEX`) **+** ~11 B por esquina (`M_BINDING`: uv, page, vref, uvw, twin) | la sala existe **tres veces**: lógica, malla de salida y binding del render |
| Cara de sala | 10 / 8 B | 20 B (`FACE`) + índices de salida | |
| Tabla de ítems | ~24 B × nº ítems (≤ 250) | 184 K fijos (`MAX_ITEMS` 1024 × `ITEM`) | tamaño máximo siempre |
| Fotogramas | 271–425 K | ~424 K | ya empaquetados (3 ángulos en `uint32`); sin compresión |

**D0 medido (02/11/2026, DC builds 413–415, censo, pasadas de niveles sin
películas; detalle en REGISTRO)**: bytes por `GBUF_*` al terminar la carga
(`[gbuf]`, `tools/dreamcast/gbuf_summary.py`), últimos ~10 niveles de cada
juego:

| K en RAM | TR1 | TR2 | TR3 | Fichero |
|---|---:|---:|---:|---:|
| **Fotogramas** (`anim_frames`) | **1 260–1 400** | **1 280–1 400** | **1 230–1 310** | 270–425 |
| Sectores | 150–390 | 330–870 | 150–600 | 26–118 |
| Malla de salas | 155–375 | 150–575 | 110–595 | 85–370 |
| Mallas de objeto | 230–300 | 290–345 | 305–425 | 66–181 |
| Ítems | 180 | 180 | 180 | ~5 |
| **Total del nivel** | 2,3–2,8 MB | 2,5–3,5 MB | 2,3–3,4 MB | 0,6–1,2 MB |

- **Los fotogramas son lo primero**: ~1,3 MB en todos los niveles de los tres
  juegos (16 000–17 000 fotogramas por nivel, ~18 000–20 000 con los que
  reservan las inyecciones). Cada uno es un `ANIM_FRAME` de 24 B más una
  reserva propia de 4 B por pieza (`M_ParseFrame`, `anims/frames.c`); el
  fichero guarda la rotación en 2 B (un eje) o 4 B. → **D4 pasa a ser:
  guardar el flujo crudo de int16 del fichero + un índice por fotograma, y
  decodificar caja/desplazamiento/rotaciones al leer** (`Anim_GetFrameRots`
  ya decodifica a un búfer). Estimado: 1,3 MB → ~0,5 MB.
- Las mallas de render (`OUTPUT_MESH`, bindings del batcher) **no salen** en
  el volcado de fin de carga: se hacen después. Falta medirlas aparte.
- **Newlib** (fuera de región y Lua, ~2,4 MB): catálogos ~280 K
  (`Catalog_CreateKey`, `Catalog_BindSlot`, `M_AddName`), `M_FindOrAdd`
  110–170 K, vectores `M_EnsureCapacity` 110–340 K, JSON de configuración vivo
  tras leerlo 80–90 K, cadenas (`vsnprintf`, `JSON_ValueGetString`) ~120 K,
  mapas de enums ~70 K, `UI_InitText` 34 K, `TestReplay` ~40 K, y ~670 K sin
  nombre (marcos no resueltos). Censo: `live_census.py <log> <elf>
  <addr2line> --tag nlive`.

| # | Cambio drástico | Ganancia estimada (nivel grande) | Riesgo / esfuerzo |
|---|---|---:|---|
| D0 | **Medir**: contador de bytes por `GBUF_*` y por binding del batcher impreso al terminar la carga (`game_buf.c`, `batcher.c`). Un día. | — | bajo |
| D1 | **Una sola copia de cada sala**, en formato del PowerVR, hecha en el horno (fase B): vértices compartidos de 8 B (xyz 16 bits + color/sombra 16 bits), caras como índices de 16 bits + textura. Se tira `ROOM_MESH` de la lógica (la lógica no mira la geometría de sala; lo que la use —`shadows.c`, capi de Lua, editores de inyección— se resuelve en el horno o lee la copia del render). | −0,8 … −1,5 MB | alto; toca `batcher.c` y la carga |
| D2 | **Sectores como en el motor original**: guardar los 8 B del fichero y decodificar suelo/techo/disparadores **al consultar** (`Room_GetHeight` & cía. ya pasan por pocas funciones). | −0,3 … −0,6 MB (TR3 Roofs: 15 039 sectores × 48 B) | medio; CPU extra pequeña en colisión |
| D3 | **Tabla de ítems a la medida del nivel** (ítems del fichero + margen para los creados en juego, p. ej. +64) en vez de 1024 fijos. | −120 … −150 K | bajo |
| D4 | **Fotogramas comprimidos** (delta por animación + cuantizado) y decodificar solo los de los ítems activos, con caché pequeña. | −150 … −250 K | medio; CPU por frame |
| D5 | **Lua fuera de la partida**: el estado de Lua de la UI (menús, pasaporte, Graphics) se crea al abrirlos y se destruye al volver al juego; lo que Lua hace por frame en partida (HUD, eventos `[luaev]`, `TICK` ~0,9 ms) pasa a C. Antes: inventariar qué scripts corren en partida. | hasta −2,3 MB en partida (la región entera de Lua) | **muy alto**: reescribir la UI de juego en C |
| D6 | **Un ejecutable por juego** (TR1/TR2/TR3), el selector lanza el suyo con `arch_exec`: fuera el código, tablas y catálogos de los otros dos y de TR4. | −0,3 … −0,6 MB (`.text`+`.rodata`+`.bss`) | medio; tres builds y el selector |
| D7 | **Cadenas y catálogos horneados**: el ~2,2 MB de bloques pequeños de newlib (tras A5) a un único bloque de solo lectura por juego, leído del disco, sin `strdup` por entrada. | hasta −1 MB (sin medir) | medio |

Con D1+D2+D3 el nivel cargado bajaría de 2,5–3,6 MB a ~1,2–2 MB en los tres
juegos; con B el pico de carga queda igual que eso. La región del nivel podría
pasar de 6,6 MB a ~3 MB y devolver más de 3 MB al resto (Lua, newlib, margen).
D5 es la palanca más grande pero la más cara; decidirla después de D0 y A5.

## Rama `refactorizacion-masiva`: hecho (02/11/2026)

Worktree `D:\trx\trx2-dc-refactor`. Pasadas de niveles sin películas en
Flycast, oculto y sin sonido (builds del worktree 1-3, logs
`build/logs/r1-tr*.txt`): **TR1 16, TR2 18 y TR3 20 niveles con
`[levels] done`**, sin aserto ni excepción.

| Cambio | Ganancia medida |
|---|---|
| D4 fotogramas en palabras de TR2, depósito por nivel, solo los usados | −185…−215 K por nivel (Barkhang 1376 → 1162 K, Atlantis 1316 → 1130 K); menos de lo estimado: la mayoría de rotaciones giran en dos ejes y siguen en 4 B |
| `MAX_ITEMS` 1024 → 512 | ítems 180 → 90 K por nivel, y tablas estáticas a la mitad |
| Tiempo 512, cola de relieve 256, chispas 256 | `.bss` 729 → 602 K (junto con lo anterior) |
| Parte grande de Lua 1024 → 512 K | +512 K para el resto |
| Sin pcre2 | `.text` −98 K |
| **Total** | región del nivel **6336 → 7231 K** (TR1 7295 K); datos de Barkhang 3391 → 3077 K; pico de carga máx. TR1 5679 K, TR2 6034 K, TR3 6212 K (78-86 % de la región; antes 87-100 %) |

Descartado al probarlo:

- `SURFACE` con altura de 16 bits (sector 56 → 52 B): TRX usa
  `NO_HEIGHT` = −3 251 200, no cabe; traducirlo toca ~70 accesos por 4 B.
- Soltar el JSON de configuración (80-90 K): se guarda a propósito, lo
  consultan las opciones que se registran más tarde.
- Compartir fotogramas repetidos: 29-52 K por nivel en TR2 (el fichero trae
  ~7000 fotogramas; los otros ~10 000 son las animaciones de Lara que
  inyecta TRX, iguales en todos los niveles).
- Código solo de TR4: ~7 K, no compensa un ejecutable por juego por eso.

## Rama `refactorizacion-masiva`: segunda tanda (02/11/2026, sin probar)

Compilado, no probado: A2 (`.bss` 602 → 498 K), A3 (`-Os` en carga/menús),
C1/C2, D2 reducido (`SECTOR` 56 → 48 B), D7 parcial (catálogo sin cadenas
duplicadas) y C4. Pendiente: B, D1, D2 completo, D6, D7 completo y C3. Detalle
y motivos en [REGISTRO.md](REGISTRO.md) y TAREAS 75.

## 5. Orden propuesto

A5 + D0 (medidos el 02/11) → **D4 (fotogramas, ~0,8 MB)** → D3 (ítems) → A1
→ A2 → A3/A4 → C1/C2 → B junto con D1/D2 (el formato horneado ya lleva salas
y sectores compactos) → D6/D7 → D5 si sigue haciendo falta → C3/C4.
La fase A sola debería subir el margen de `brk` de ~100 K a ~1 MB en Barkhang;
la B es la que quita el problema de raíz en la carga y abre sitio para todo lo
demás.

## 6. Descartado

- Streaming de salas/animaciones en partida (ver 3).
- Migración a arenas con época (TAREAS 67): no hace falta para esto; revisar
  solo si tras A+B sigue habiendo fugas entre juegos.
- Paleta de 8 bpp de OpenLara: solo VRAM, y VQ ya es mejor.
