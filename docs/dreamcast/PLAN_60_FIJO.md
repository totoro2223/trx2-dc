# Plan: 60 fps fijos en consola (render nativo de DC + lógica en su hilo)

Preparado el 09/11/2026. Nada empezado ni medido: todas las cifras de este
documento son **estimaciones** hasta la fase 0. Antes de empezar lee
[TRASPASO.md](TRASPASO.md), [PROBADO.md](PROBADO.md) (lo FALLIDO no se repite
sin un dato nuevo), [REA_TR3_PSX.md](REA_TR3_PSX.md) y
[optimizacion render.md](optimizacion%20render.md).

## Estado (09/11/2026, rama `full60fps`)

Fases 0-7 hechas en código y probadas **solo en Flycast** (oculto, DC builds
842-851). Falta lo que decide el plan: medir en GDEMU.

| Fase | Hecho | Dónde |
|---|---|---|
| 0 | Vista 2 del HUD: `LOG/STG/PAS` y `PVR/MAX` (media de 30 frames y peor frame), en la build normal | `output_common.c` (`g_DC_PhaseUs`), `nolua_hud.c` |
| 1 | Salas horneadas al cargar: cada trozo en lotes de ≤256 vértices únicos; por frame, cada vértice se transforma una vez y las caras se emiten leyendo por un mapa (`dc_emit_run_map`). Las salas con sprites (TR3) y con esquinas compartidas entre trozos ahora también se trocean (copia de la esquina) | `batcher.c` (`M_BakeNative`, `M_DrawNative`, `M_SplitShared`), `emit_sh4.S` (`DC_EMIT_MAP`), `emit_map_sh4.S`, `xform_sh4.S` (sin `vref`) |
| 2 | El búfer del lote en la OCRAM (`__ocram`, 0x7C001000, 8 KB) con `dcache_toggle_ocram`, solo en el modo nativo; escrituras sin `movca` | `batcher.c` (`DC_Batcher_SetNative`) |
| 3 | Mallas: ya tenían vértices únicos (gemelos), pose a 30 Hz y mezcla lineal; nuevo: niebla por malla con la distancia del origen (tinte) cuando no hay tabla | `batcher.c` (rama de objetos) |
| 4 | Hilo de render (prioridad 8) cada 1/60 s con el cerrojo del mundo; la lógica corre los ticks a 30 Hz en el hilo del ejecutor y cede entre ítems, tras Lara y tras efectos; los frames dibujan los dos últimos ticks enteros (cámara, Lara, ítems, efectos y trenza publicados al cerrar cada tick); una fase anidada (inventario) pausa el hilo | `dc_world.c`, `executor.c` (`M_RunThreaded`), `interpolation.c` (`Interpolation_DC_*`), `items/manager.c`, `game/control.c` |
| 5 | Tope: pasados 2500 polígonos en el frame, fuera los objetos que no son sala ni Lara a más de 4 casillas | `batcher.c` (`M_NATIVE_POLY_CAP`) |
| 6 | Niebla negra por tabla de la PVR, por profundidad (como el PSX), 12 → 20 casillas, solo salas y objetos del batcher; sin niebla por vértice | `pvr.c` (`DC_PVR_SetupFogTable`), `batcher.c` (`fog_table`) |
| 7 | Modo «Native 60 FPS» (`DC_MODE_NATIVE_60`, `native-60`, `TRX_DC_MODE=native-60`): 640×480, 60, sombra del juego, sin cel ni relieve | `dc_mode.c`, `enum.*`, `entries.def`, `nolua_settings.c`, `nolua_hud.c`, `make_disc.sh` |

Comprobado en Flycast:
- **Lotes contra la ruta de salas:** en las primeras 120 frames de cada arranque, cada vértice de la OCRAM se compara con la ruta de salas de siempre. Resultado: TR2 96 075 y TR3 66 759 esquinas, 0 distintas.
- **Frecuencia:** 60 fps estables en TR2 Great Wall y TR3 Jungle.
- **Salida del nivel:** con el hilo activo se sale bien (`TRX_DC_EXIT`).
- **Modo Graphics:** sigue a 30 fps sin hilo.

En Flycast (que no modela la caché):

| | `pass` antes | `pass` ahora | Vértices antes | Vértices ahora |
|---|---|---|---|---|
| TR2 | 4,1 ms | 3,7 ms | 3644 | 3017 |
| TR3 | 6,9 ms | 5,3 ms | 4711 | 3456 |

**Antártida (TR3, nivel 16)**, al empezar el nivel, en Flycast (DC builds
852-855):

| | Native 60 | Graphics |
|---|---|---|
| fps | 36 | 30 |
| `stage` | 6 ms | 6 ms |
| `pass` | 18 ms | 22 ms |
| Vértices / polígonos | 10 500 / 1525 | — |

- **El límite es el render:** unos 25 ms por frame, con la lógica a 30 ticks por segundo.
- **Fallo corregido:** con un frame más largo que 16,6 ms, el hilo de render, de más prioridad, dejaba sin CPU a la lógica: 5 ticks en 600 frames, juego congelado a 43 fps. Ahora, si la lógica va atrasada más de un tick, el render espera a que acabe uno: se pierden frames, nunca ticks.
- **Espera del render por el cerrojo:** 0,1 ms de media y 2,4 ms como mucho. Con una sola CPU, desacoplar más no da CPU: lo que falta para 60 es bajar el `pass` (10 500 vértices para 1525 polígonos; lotes de unos 36 vértices) y el `stage`.
- **Log:** cada 600 frames, `[world]` da ticks, frames tarde, espera del cerrojo, dibujo y duración del tick.

**Segunda ronda sobre el `pass` (09/11/2026, DC builds 856-876)**, en
Flycast:

| Escena | fps | `pass` | Antes |
|---|---|---|---|
| Antártida | 39-40 | 15,8 ms | 36 fps, 18,1 ms |
| Jungla | 60 | 5,5 ms | — |
| TR2 Great Wall | 60 | 3,9 ms | — |

- Verificación: 0 distintas en las tres escenas (la Antártida, 1 097 775 esquinas).
- **Reparto medido en la Antártida antes de los cambios:** las salas, unos 10,6 ms entre transformación, emisión y coste por sala; los objetos (66, casi todos estáticos), 5,1 ms; el descarte de trozos, 0,56 ms. Por vértice ~0,8 µs y por cara ~1,5-2 µs: domina el trabajo por vértice y por cara, no el coste fijo.
- **Hecho:**
  - **Proyección a pantalla una vez por vértice:** `dc_nat_screen` (`nat_sh4.S`) calcula centro, escala y profundidad de la PVR por vértice único; la emisión con mapa los copia. Es lo que más ha dado.
  - **Color horneado por lote** (`dc_nat_place`, `M_NatKey`): se reutiliza mientras no cambien el tinte, la curva, la sombra mínima ni el parpadeo, y no haya luces dinámicas ni cáusticas. Los lotes con sprites o con vértices que mueve el agua se iluminan cada frame. En la Antártida es el 35-50 % de los lotes; ahorra 0,9 ms.
  - **Salas pequeñas por el nativo, como un solo trozo**, y **trozos de 4096 unidades** en vez de 2048: unos 140 lotes por frame en vez de unos 370.
  - **Hilo de render despertado por la vblank** (semáforo en `vblank_handler_add`) en vez de `thd_sleep`: los frames que empiezan tarde bajan de 398-504 a 7-9 cada 600, y esos son de la carga.
  - **Corregido:** la niebla por tabla no entraba en la clave de la caché de cabeceras ni en el estado de cada lista (`M_SameState`), así que una partícula podía heredar la niebla de la sala anterior.
- **Probado y quitado:** separar en cada trozo las caras normales, las de agua y las de sprites (más cambios de página, sin ganancia).
- **Lo que queda en la Antártida:** `stage` 6 ms y `pass` 16 ms; los estáticos se dibujan uno a uno.

**Tercera ronda (09/11/2026, DC builds 877-883)**, en Flycast:

| Antártida, Native 60 | Antes | Ahora |
|---|---|---|
| fps | 39-40 | 43-44 |
| `stage` | 6,0 ms | 4,5 ms |
| `pass` | 15,8 ms | 14,8 ms |
| Instancias | 93 | 60 |

La Jungla y TR2 Great Wall siguen a 60. Verificación: 0 distintas en las tres escenas.

- **Estáticos fusionados en su sala al cargar** (`rooms.c`, `M_MergeStatics`), en todos los modos:
  - Las caras del estático pasan a ser de la sala, giradas y colocadas como hace `Matrix_RotY`.
  - Se iluminan como las ilumina el juego en reposo: en TR3, el ambiente RGB15 por la sombra propia del vértice; en TR1/TR2, el sumando de la sombra más la del vértice. Una luz dinámica cercana los ilumina ahora por vértice, como a la sala.
  - Al dibujar se saltan los fusionados (`rooms/draw.c`).
  - Quedan fuera los que tienen caras planas, semitransparentes o reflejos, los rompibles de TR3 (50-59) y los de salas de luz animada en TR1/TR2.
  - Tope de 24 000 vértices fusionados por nivel, por la memoria.
  - En la Jungla se fusionan 13 310 vértices; en TR2 Great Wall casi ninguno (sus estáticos no cumplen las condiciones).
  - El color no se ha mirado en pantalla.
- **Esfera y cono de normales por lote** (como los *meshlets*): el lote fuera del frustum o con todas las caras de espaldas no se transforma. El signo del cono está comprobado: en los frames de verificación, de 833 lotes que habría descartado, ninguno dibuja nada. En la Antártida descarta unos 7 lotes por frame: casi todo lo que se transforma es visible.
- **Lo que queda:** el `pass` (unos 3 ms de transformación nativa y unos 6 de emisión) es trabajo por vértice y por cara de geometría visible. El cambio grande que falta es preparar la escena (`stage`) una vez por tick en vez de por frame, con la interpolación hecha en el `pass`.

**Cuarta ronda: la escena se prepara una vez por tick (09/11/2026, DC builds 884-886)**, en Flycast:

| Antártida, Native 60 | Antes | Ahora |
|---|---|---|
| fps | 43-44 | 46-47 |
| `stage` (media por frame) | 4,5 ms | 3,0-3,2 ms |
| `pass` | 14,8 ms | 14,8 ms |

La Jungla y TR2 Great Wall siguen a 60.

- **Cómo funciona** (`dc_world.h`, `DC_Replay`; `compositor.c`, `batcher.c`):
  - El primer frame tras un tick prepara el mundo (`Game_Draw`) una vez, con el estado exacto del tick (`frac` 1).
  - Cada flush de esa preparación (el cielo, el mundo) se guarda como segmento: instancias, partículas, quads, gotas, sombras, modo cielo y los bloques de uniformes.
  - Ese frame se dibuja a medio camino del tick anterior. La cámara y la matriz de mundo de cada instancia se mezclan con las del tick anterior: las filas del giro interpoladas y normalizadas, la posición a medias. Cada instancia se empareja por malla, segmento, pass y orden de aparición.
  - Los frames siguientes del tick no preparan nada: vuelven a dibujar los segmentos tal como los dejó el tick. La interfaz sí se prepara en cada frame.
  - Orden de lo que se ve: medio tick antes, luego el tick. Es medio tick menos de retraso que antes (antes el primer frame mostraba el tick anterior).
- **Casos en que se vuelve a preparar:**
  - Hay otra fuente en el segmento (overlay, relámpago, prismáticos).
  - Hay más de 4 flush.
  - Falta memoria.
- **Casos en que no se mezcla:** un salto de más de 2048 unidades o un giro de más de unos 45° (un corte de cámara, un teletransporte).
- **En el frame interpolado no se usa la ventana de cada sala**, porque se calculó con la cámara del tick: se recorta por la pantalla entera.
- **Medido en la Antártida, cada 300 ticks:** 294 frames repetidos, 18 600 instancias mezcladas, 0 cortes, 0 demasiado lejos.
- **Límites conocidos:**
  - Las partículas, quads y gotas no se mezclan: se ven en el estado del tick con la cámara a medio camino.
  - Una sala que se vería con la cámara a medio camino pero no con la del tick falta en ese frame (en un borde de la pantalla, en giros rápidos).
  - Las luces dinámicas son las que haya al dibujar.

**Quinta ronda sobre el `pass` (09/11/2026, DC builds 887-894)**, en Flycast:

| Antártida, Native 60 | Antes | Ahora |
|---|---|---|
| fps | 46-47 | 54-55 |
| `pass` | 14,8 ms | 12,0-12,3 ms |
| Polígonos por frame | 1690 | 1510 |
| Lista opaca | 147 KB | 127 KB |

La Jungla y TR2 Great Wall siguen a 60. Verificación: 0 distintas (Antártida 1 220 868 esquinas, Great Wall 188 130).

- **Desglose medido con temporizadores baratos** (logs `[pass]`): las salas son 11,5 ms del `pass` y los objetos 3,2 ms; partículas y gotas no llegan a 0,2 ms.
  - Dentro de las salas: transformación nativa 3,1 ms, emisión 4,3 ms, vértices fuera de los trozos 1,2 ms, y el resto en preparación por sala.
  - De la transformación nativa, 2,6 ms eran lotes con agua (70 por frame) por la ruta completa.
- **Agua en el camino horneado, como el PSX** (`M_NatWater`, `M_NatWaterPlaces`, `M_WaterTable`):
  - Al hornear un lote se guarda, por vértice de agua, su índice en la tabla de 64 entradas (franja de su sitio en el mundo × efecto).
  - Por frame: el color horneado, y en los vértices de agua su color más el valor de la tabla, saturado, por la curva y el tinte, como la ruta de sala en ensamblador.
  - Todos los lotes van ahora por el camino horneado. Transformación nativa 3,1 → 1,8 ms.
- **Corregido: en el pase de suma, las salas con caras de suma (7 por frame en la Antártida) volvían a transformar y emitir sus trozos opacos.** Cada cara opaca de esas salas iba dos veces a la lista.
  - Ahora ese pase solo transforma los vértices fuera de los trozos (`m_BlendPass`).
  - Quita 180 polígonos por frame, en todos los modos.
- **Vértices fuera de los trozos por el camino horneado** (`M_NatAlways`): las caras transparentes y de suma (la superficie del agua, el hielo).
  - Sus vértices se colocan una vez cada uno con el color horneado (y el agua), y luego se reparten por esquinas.
  - Se comprueban contra la ruta de sala en los frames de verificación.
  - Va por ahí en 14 de las 33 salas dibujadas por frame. Las demás tienen sprites, o esquinas cuyo vértice es de un lote, y siguen por la ruta de la sala.
- **Lo que queda en la Antártida:**
  - La emisión (unos 3,5 ms).
  - Los objetos (3,2 ms).
  - La preparación por sala (unos 1,3 ms para unas 33 salas por frame).

**Sexta ronda: los objetos (09/11/2026, DC builds 895-898)**, en Flycast:

| Antártida, Native 60 | Antes | Ahora |
|---|---|---|
| fps | 54-55 | 55-58 |
| `pass` | 12,0-12,3 ms | 11,7-12,0 ms |
| Transformación de objetos | 1,72 ms | 1,34 ms |

La Jungla y TR2 Great Wall siguen a 60.

- **Desglose medido:** 28 objetos por frame (Lara, enemigos, recogibles) con 2614 vértices, por la ruta normal en ensamblador. Transformación 1,4 ms más 0,3 de preparación, emisión 1,45 ms.
  - Un vértice de objeto costaba unas 3 veces uno de sala, porque se ilumina en cada frame.
- **Colores de objeto guardados** (`M_ObjKey`, `M_ObjCached`, `dc_obj_place` en `nat_sh4.S`):
  - El color de cada esquina de un objeto depende de su luz en su propio espacio, del tinte y de la sombra mínima. Con la misma clave, se reutilizan los colores de la vez anterior y solo se colocan las esquinas, con sus gemelos copiados como en `dc_xform_object`.
  - En el frame interpolado, la luz se calcula con la matriz exacta del tick, guardada en `cwmatrix` (el render de DC no la usa). Así los frames siguientes del tick aciertan siempre.
  - Un objeto quieto (un recogible, una puerta) acierta de tick en tick.
  - La clave ignora los 8 bits bajos de cada número, porque la luz del frame interpolado se calcula a través de otra vista.
  - Quedan fuera: niebla por vértice (fuera de Native 60), cel, reflejos, mallas que se deforman y cáusticas.
- **Medido:** en la Antártida se reutilizan 11 158 de cada 16 800 objetos (69 %).
- **Verificación:** en los frames de comprobación, los colores guardados se comparan con los de la ruta normal (`[obj-check]`). Antártida: 108 178 esquinas; Great Wall: 13 656. 0 diferencias de más de un nivel por canal.

**Séptima ronda: la emisión (09/11/2026, DC builds 899-903)**, en Flycast:

| Antártida, Native 60 | Antes | Ahora |
|---|---|---|
| fps | 55-58 | 57-60 |
| `pass` | 11,7-12,0 ms | 11,4-11,5 ms |
| Emisión nativa | 3,46 ms | 3,26-3,29 ms |

La Jungla y TR2 Great Wall siguen a 60. Verificación: 0 distintas (Antártida 1 221 084 esquinas; objetos 112 390 en la Antártida y 11 380 en Great Wall).

- **Medido:**
  - La preparación de cada sala cuesta poco: 0,36 ms entre todas.
  - El peso está en la emisión: nativa unos 3,5 ms, objetos 1,5 ms, pase de suma 0,6 ms.
  - El emisor ya está en ensamblador: unas 40 instrucciones por vértice y unas 30 por cara de recorte.
- **Emisor de lotes nativos (`DC_EMIT_MAP`), 12 instrucciones menos por vértice:**
  - Sin color de niebla en el lote, el color de desplazamiento es 0 sin calcularlo. En Native 60 ningún vértice nativo lleva la marca de niebla: la niebla es la tabla de la PowerVR.
  - La posición en pantalla se guarda sin copiarla antes a otros registros.
- **Temporizadores finos de esta sesión** (`[pass]`): costaban unas mil lecturas de reloj por frame. Ahora solo se compilan con `TRX_DC_PASS_TIMERS`.
- **Lo que queda en el emisor:**
  - **UV:** pasar a 16 bits en el formato de la PowerVR ahorraría unas 7 instrucciones por vértice. Pero ese formato pierde el desplazamiento de medio texel del bilineal en la mitad derecha de cada página, así que no se hace.
  - **UV en coma flotante precalculadas por esquina:** cuestan unos 4 bytes más por esquina de memoria.
  - Más allá de eso, bajar la emisión es emitir menos caras.

Sin probar:
- La consola.
- La imagen (en modo oculto no hay capturas).
- TR1.
- El inventario con el hilo activo.
- Guardar con el hilo activo: `TRX_DC_SAVE` no guarda sin Lua.

Diferencias con el plan: los vértices siguen en el formato de 28 B (se lee uno por vértice único, no por esquina), y los lotes salen pequeños (unos 10 vértices de media), porque los trozos son de 2048 unidades.

## Objetivo

60 fps **fijos** en consola real (GDEMU), en el peor caso de TR3 (Antártida,
Londres), en un modo propio ("60 nativo"). El render actual se queda como está,
detrás de un interruptor, para comparar y como reserva.

## Reglas (no se negocian)

- **La niebla no se toca**: distancia del original (PSX, 12 → 20 casillas).
- **Copiar el método del PSX** (decompilado en `D:/trx/_rea/tr3psx`), no
  inventar.
- **Lógica en ticks fijos de 1/30 s** (la lógica a 15 Hz ya fue rechazada).
- **Medir en consola**: Flycast no modela la caché, y la ganancia de este plan
  está justamente en la memoria. Una fase que no gane en GDEMU se deshace y se
  anota como FALLIDO.
- Todo lo nuevo, en `src/platform/dreamcast/`; en código compartido, bajo
  `#if defined(__DREAMCAST__)`.

## Por qué hace falta un cambio de arquitectura

| | Hoy (consola) | Para 60 fijos |
|---|---|---|
| Frame | ~55 ms (17-18 fps, Jungle y demo de TR1) | 16,6 ms |
| Coste por vértice | ~4 µs (benchmark build 750) | 0,3-0,5 µs (PSX: 0,23) |
| Lógica | entera en uno de cada dos frames a 60 | repartida entre los dos |

- El render recorre la escena **dos veces** (`stage`: capa `Output_*` de
  TRX; `pass`: el batcher) sobre la malla genérica del motor (28 B por
  **esquina**, unas 2,7 esquinas por vértice; luz y niebla recalculadas en
  cada frame).
- A 60 fps con lógica a 30 Hz, un frame lleva lógica y render y el siguiente
  solo render: aunque el promedio quepa, el frame con lógica se pasa.
- El PVR no es el límite (600-2.000 polígonos por frame a 60 es poco para él).
- Lo micro ya está hecho y en ASM (MIGRACION_ASM.md); las store queues y el
  `pref` no ganaron en consola. Queda la forma de los datos y el orden del
  trabajo.

## Presupuesto objetivo (consola, por frame a 60)

| Parte | ms |
|---|---|
| Render de salas | 2,5 |
| Render de mallas (Lara, enemigos, objetos) | 1,5 |
| Partículas, HUD, emisión restante | 1,5 |
| Lógica (un tick de ~10 ms repartido en dos frames) | 5 |
| Copia del estado, sincronización, margen | 3 |
| **Total** | **13,5** de 16,6 |

## Fases

Cada fase termina con su medida en GDEMU, una fila en PROBADO.md y el detalle
en REGISTRO.md. **Fase 0 y 1 deciden si el plan sigue.**

### Fase 0. Medir en consola lo que cuesta cada parte

- Build `release` con el reparto `logic/stage/pass/pvr` en pantalla (hoy solo
  sale en la build de sondas; la vista 2 del HUD ya muestra fps, polígonos y
  vértices: añadir las fases, sin puerto serie).
- Medir en GDEMU: Antártida (vista inicial), Londres (primer enemigo), Jungle,
  demo 1 de TR1. Anotar el coste por vértice real.
- **Criterio**: saber cuánto es `stage`, cuánto `pass` y cuánto lógica en
  consola. Si la lógica sola pasa de ~12 ms por tick, la fase 4 no basta y hay
  que atacar la lógica antes (ANALISIS_COSTE_IA_TRX.md).

### Fase 1. Prototipo: salas horneadas al cargar (la prueba de fuego)

Solo salas, detrás de un interruptor (`dc_native_rooms`, apagado por defecto).

- **Horneado al cargar** (sin `.DCL`), desde `M_BuildChunks`/`M_ShareVertices`,
  que ya trocean y juntan los vértices: por trozo, hasta 127 **vértices únicos**
  de 8-12 B (xyz en int16 relativos al trozo, color estático ya calculado,
  bandera de "luz variable"), caras como índices de 8 bits agrupadas por página,
  UV en formato de la PVR, cabecera de página precalculada. Formato del PSX
  (REA_TR3_PSX.md, punto 5).
- **Por frame y por trozo visible**: `ftrv` de los vértices únicos a un búfer
  pequeño; emitir las caras leyendo de ese búfer. El color estático se copia
  tal cual; solo los vértices marcados pasan por las luces dinámicas, el agua y
  el parpadeo (lista de luces por trozo, como el PSX en `0x1f800300`).
- Reusa lo que ya hay: recorrido de portales y ventanas (`rooms/draw.c`), el
  descarte de trozos, la proyección y los bits de recorte del asm actual.
- **Criterio de éxito**: en GDEMU, el coste por vértice de sala baja **al
  menos 3×** frente al render actual en las mismas cuatro escenas, con la
  imagen igual (captura comparada). Si no, el plan se para aquí: FALLIDO en
  PROBADO con la cifra.

### Fase 2. Scratchpad: media caché de datos como RAM

- Encender `CCR.ORA` (8 KB en `0x7C000000`; la caché normal baja a 8 KB) y
  poner ahí la matriz, la lista de luces del trozo, las tablas y el búfer de
  vértices transformados del trozo (127 × 32 B ≈ 4 KB). Es el `0x1f800000` del
  PSX.
- Comprobar antes qué toca `CCR` en KOS (`kernel/arch/dreamcast/kernel/cache.s`)
  y que nada más use esa mitad.
- **Criterio**: gana en GDEMU frente a la fase 1 sin scratchpad. Si no, fuera.

### Fase 3. Mallas en el mismo formato

- Lara, enemigos, ítems y estáticos con vértices únicos empaquetados y su luz
  (prelit donde el original lo hace; luz por cara a menos de 3 casillas, como
  la bandera `0x100` del PSX).
- La pose se calcula a 30 Hz (en el tick); en los frames intermedios solo se
  mezclan matrices en lineal (`dc_linear_pose`, ya por defecto, `dc_blend12`).
- Niebla por malla con la z del origen (PSX, punto 7 de REA_TR3_PSX.md).
- **Criterio**: `vobj` en consola baja al menos 2× con la misma imagen.

### Fase 4. Lógica en su hilo, estado en doble búfer

- El tick de lógica (`Game_Control` y lo que cuelga) pasa a un hilo de KOS de
  **prioridad baja**; el render, en el hilo principal, de prioridad alta, se
  despierta en cada vblank.
- El render dibuja interpolando entre los ticks N-1 y N con una **copia** del
  estado que necesita (posición, giro, pose y frame de cada ítem visible,
  cámara, Lara, partículas): TRX ya guarda el anterior y el actual para
  interpolar (`interpolation.c`, `Interpolation_Remember*`); la copia se hace
  al cerrar cada tick, con un cerrojo corto.
- La lógica calcula N+1 en el tiempo que deja el render (unos dos frames).
- **Coste**: un tick (33 ms) más de retardo en el mando; la copia del estado.
- **Riesgos**: todo lo que el render lee hoy del estado vivo durante el
  dibujo (salas visibles, `draw_rooms`, efectos, el HUD) tiene que salir de la
  copia; carreras con el sonido y la VMU. Comprobar que el planificador de KOS
  reparte el tiempo entre hilos como hace falta (prioridades y `thd_pass`).
- **Criterio**: en GDEMU, ningún frame del recorrido de prueba por encima de
  16,6 ms por culpa del tick (histograma de `frame ms`), y las demos acaban en
  su sitio (log `[lpos]` con `-DTRX_DC_LPOS`).

### Fase 5. Tope de polígonos del peor caso (como el PSX)

- El PSX tiene un tope de ~2.000 polígonos y deja de añadir al llenarse. Aquí,
  ordenado: al pasar del presupuesto, soltar primero lo lejano o pequeño
  (sprites de efectos, estáticos e ítems pequeños), nunca salas ni Lara.
- **Criterio**: Antártida y Londres a 60 fijos en GDEMU.

### Fase 6 (opcional). Niebla de la PVR por tabla

- Solo con el visto bueno del usuario: la tabla va por profundidad, como el
  PSX, pero cambia la imagen respecto a la distancia radial de hoy. Distancia
  12 → 20 casillas sin cambio.

### Fase 7. Disco y modo

- Modo "60 nativo" en el menú (Gráficos): render nativo, lógica en su hilo,
  sin relieve, sin cel, sombra octágono (como el modo Original).
- Si el horneado al cargar pesa en el tiempo de carga, moverlo a
  `make_disc.sh` (enlaza con la fase B, `.DCL`, de PLAN_REFACTOR_RAM.md).

## Qué no entra

- Tiras largas: en TR cada cara usa su tile y las esquinas no comparten UV.
- Caché de vértices entre frames: la cámara casi siempre se mueve.
- Relieve, cel shading y sombra suave en el modo 60 nativo (siguen en los
  demás modos).
- Acortar la niebla o la distancia de dibujo.

## Riesgos generales

- **Fidelidad**: la luz de TRX (TR1/TR2 de un canal, TR3 con color, agua,
  parpadeo, tintes, sombreado mínimo) tiene que salir igual. Se valida trozo a
  trozo comparando con el render actual (como el `DC_EMIT_VERIFY` del asm).
- **Esfuerzo**: semanas. Por eso las fases 0 y 1 van primero y pueden parar el
  plan en días.
- **Dos renders a mantener** mientras dure la comparación.

## Orden

0 → 1 (decide) → 2 → 4 → 3 → 5 → 7; la 6 cuando el usuario decida. La 4 va
antes que la 3 porque, sin ella, 60 fijos no es posible aunque el render
quepa.
