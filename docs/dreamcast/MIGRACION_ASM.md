# Migraciones a ASM (SH-4): hecho y pendiente

Estado al 06/11/2026 (rama `dreamcast`). Orden del usuario: todo lo que corra por
fotograma y tenga un camino rápido en ASM que dé el mismo resultado pasa a ASM, sin
descartar una pieza por "ganar poco". El C de cada rutina queda como referencia: en una
build con `dc_emit_verify=true` (`DC_EMIT_VERIFY`) se corren los dos y se comparan; en la
build normal solo corre el ASM. En las otras plataformas queda el C.

## 1. Hecho

| Rutina (archivo) | Qué reemplaza | Verificación contra el C | Ganancia medida |
|---|---|---|---|
| `dc_xform_room`, `dc_xform_object`, `dc_xform_own` (`xform_*_sh4.S`) | Transformación, luz y niebla de vértices de salas, objetos y mallas con luz propia | `[xform-verify]`: 0 distintas | base de las mejoras anteriores |
| `dc_emit_run` (`emit_sh4.S`) | Emisión de caras (cuadros y triángulos) al PowerVR; con luces por cara en salas cel (`emit_quad_l`/`emit_tri_l`, `faceaway`) | `[emit-verify]`: 0 distintas (más de 7 millones de índices); luces por cara **sin verificar** (solo corren con salas cel, apagadas en los cuatro modos) | — |
| `dc_draw_particles`, `dc_draw_quads`, `dc_draw_streaks` (`particles_sh4.S`) | Dibujo de partículas, cuadros y trazos | `[particle-verify]`: diferencias de ~1e-5 en el tipo 2 (aproximación de `fsrra`), dentro de tolerancia | — |
| `dc_stage_sparks` (`sparks_sh4.S`) | Armado de las 256 chispas en la lista de partículas | `[spark-verify]`: 0 distintas, 225 248 partículas | armado con bengala 724 → 341 us |
| `dc_spark_control`, `dc_spark_lights` (`sparks_ctl_sh4.S`) | Lógica de cada chispa (`Sparks_Control`) y pase de luces dinámicas de las chispas | `[spark-ctl-verify]`, `[spark-light-verify]`: 0 distintas en 2 100 ticks | ~0,3 ms cada una con bengala |
| `dc_rain_update`, `dc_snow_update`, `dc_water_update`, `dc_rain_draw`, `dc_snow_draw`, `dc_water_draw` (`weather_sh4.S`) | Lluvia, nieve y partículas de agua: actualización y armado en las listas del batcher | lluvia y nieve (actualización) 0 distintas en TR3 niveles 1, 12 y 16; **dibujo de nieve, agua: poca cobertura** | — |
| `dc_effect_control` (`weather_sh4.S`) | Recorrido de la lista de efectos activos (`Effect_Control`) | **sin comparación** (solo recorre la lista; llama a las funciones de cada objeto) | — |
| `dc_get_bounds` + `sb_window` + `sdiv32` (`room_sh4.S`) | `M_GetBounds` y `M_SetBounds`: ventanas de portales (la vista de salas) | `[bounds-verify]`: 0 distintas en TR3 niveles 1, 16, 12 y 4 | `sbounds` en Antártida 948 → 480 us |
| `dc_fog_colours` (`room_sh4.S`) | `M_FogColours`: parte de la niebla de color por vértice | `[fog-verify]`: sin diferencias, pocas corridas (< 600) | no medida aparte |
| `dc_sight_blocked` (`room_sh4.S`) | `M_SightBlocked`: rayo de visión entre un ojo y un punto (núcleo de la oclusión) | `[sight-verify]`: sin diferencias, pocas corridas (< 20 000) | no medida aparte |

Otros cambios de esta tanda que no son ASM: caché de nodos de articulación para chispas
(`Sparks_JointCache`), tabla UV de chispas, ganchos de prueba (ver 4).

## 2. Pendiente, por coste medido

Datos de Antártida (TR3 nivel 16, modo Original, Flycast oculto, build normal): 36-37 fps,
unos 27,8 ms por cuadro, repartidos en lógica ~2,0, preparación de escena (`stage`) ~5,1,
pasada de dibujo (`pass`) ~13,5 y espera del PowerVR ~6,5. Dentro de `pass`: transformación
de salas ~5,1 ms, emisión ~3,8 ms, objetos ~1,0 + 0,75 ms y unos ~2,2 ms sin atribuir.

Aun con todo lo que sigue en ASM, lo estimado es ganar ~5 de los 27,8 ms (unos 42-45 fps).
Llegar a 60 exige además bajar la transformación y la emisión de salas, y ver qué parte
de los 6,5 ms del PowerVR se solapa con la CPU (los microsegundos de Flycast no son los de
la consola).

| Prioridad | Pieza (C) | Dónde | Coste medido / motivo |
|---|---|---|---|
| 1 | Preparación de cada instancia: `M_TransformInstance`, `M_PrepareInstanceLight`, `M_ModelView`, `M_WholeRanges` | `render/batcher.c` | gran parte de los ~2,2 ms sin atribuir de `sgflush` (67-98 instancias por vista); falta sonda propia |
| 2 | Oclusión por chunks: `M_ChunkHidden`, `M_ChunkHiddenAt` (celdas por sector), `DC_MeshBatcher_RayBlocked` | `render/batcher.c` | llaman a `dc_sight_blocked` pero el resto es C; sin sonda |
| 3 | Oclusión de portales y objetos: `M_FindOccluded`, `M_PortalHidden`, `M_ObjectHidden`, `M_SeeSlot` | `game/rooms/draw.c` | sin sonda |
| 4 | Armado de ítems y de Lara (`sitem` ~0,83 ms, `slara` ~1,14 ms) | `game/rooms/draw.c`, `output/sources/objects.c` | selección de mallas y pose por articulación |
| 5 | `post` (~1,08 ms): `Camera_Update`, `Sound_UpdateEffects`, `ItemAction_RunActive`, `FX_Control` | `game/game/control.c` | cámara (colisión, línea de visión) y sonido: miles de líneas |
| 6 | Cielo y escena: `M_DrawSkybox`, `sscene`/`ssky` (~1 ms entre los dos) | `game/rooms/draw.c`, `output/` | — |
| 7 | `ctrl` de Lara (~0,49 ms) | `game/lara/` | física y estados: muy grande |
| 8 | `M_XformGeneric`, `M_RoomCel` | `render/batcher.c` | genérico (sprites, reflejos); cel de salas apagado en los cuatro modos |
| 9 | Envoltorio de `M_EmitChunk`/`M_EmitIndices` y `DC_PVR_SetState` | `render/batcher.c`, `render/pvr.c` | el núcleo ya es ASM; el envoltorio decide páginas y estados |
| 10 | Spawns de clima, consulta de sala (`Room_GetOutsideStatus`), búsqueda de la función de control de cada objeto | `game/fx/weather.c`, `game/rooms/`, `game/objects/` | hoy el ASM las llama desde C (raras o no triviales) |

Antes de empezar una pieza de la lista: ponerle una sonda (`DC_PROBE`) y medirla en la vista
mala (Antártida nivel 16; selva nivel 1), para no adivinar el peso.

## 3. Verificación pendiente de lo ya hecho

- Subir la cobertura de `fog-verify` y `sight-verify` (más tiempo de corrida o niveles con
  niebla de color y más salas) hasta ver los contadores finales.
- Contadores de `snow-draw`, `water-update` y `water-draw`: no llegaron a imprimirse; correr
  un nivel con partículas de agua (TR2/TR3 submarinos) y nieve más tiempo.
- Luces por cara (`emit_sh4.S`): probar con `TRX_DC_CEL="1 1 0"` (sala cel) en un nivel con
  bengala, en una build de verify.
- `Effect_Control`: no se puede comparar corriéndolo dos veces (tiene efectos secundarios);
  comprobar a mano un nivel con efectos (sangre, humo, disparos) en C y en ASM con
  `TRX_DC_ASMOFF=32`.
- Todo esto en la consola real: Flycast no modela caché ni el reloj del SH-4.

## 4. Cómo se hace (y errores ya cometidos)

Flujo:
1. Escribir el ASM en `src/platform/dreamcast/render/*_sh4.S` y añadirlo a `meson.build`.
2. Dejar el C como `...C` con `[[maybe_unused]]`; en la build normal llama solo al ASM, en la de
   verify corre los dos y compara (`DC_EMIT_VERIFY`, registro `[xxx-verify]`).
3. Pasar los datos en un bloque de control (struct de palabras) con `_Static_assert` de los
   desplazamientos; llamadas de vuelta a C solo para lo raro.
4. Compilar (`build.sh minsize`), luego verify: `meson configure build/dc-minsize-probes
   -Ddc_emit_verify=true` y correr los niveles de prueba (cambiar la opción obliga a recompilar
   todo, ~670 pasos). Después devolver la opción a `false` y medir con la build normal.
5. Si algo falla: `TRX_DC_ASMOFF=<máscara>` (make_disc.sh) apaga rutinas por bit (1 ventanas de
   portales, 2 niebla de color, 4 rayos, 8 clima actualización, 16 clima dibujo, 32 efectos).
6. Registrar en `PROBADO.md`; lo que no sea idéntico al bit va a "Cambios dudosos".

Ganchos de prueba (todos en `tools/dreamcast/make_disc.sh`): `TRX_DC_GIVE`, `FIRE`, `NOHUD`,
`SHOWFPS`, `DEBUGKEYS`, `CEL`, `ASMOFF`, `TITLELIST`; el cuarto argumento de `make_disc.sh` es
el nivel de inicio. Las pruebas van siempre con Flycast oculto y sin sonido, salvo que se pida
verlas.

Reglas del SH-4 que costaron tiempo:
- Un inmediato (`add #n`, `mov #n`) va de -128 a 127: `add #128,r15` se ensambla como -128.
- `mov.l @(disp,Rn)` llega a 60 bytes; `mov.b`/`mov.w @(disp,Rn)` solo con R0 y desplazamientos
  de 15 y 30. Para más lejos: `mov #off,r0` + `@(r0,Rn)`, **que destruye R0**: no usarlo
  mientras R0 guarde un resultado (rompió las ventanas de portales).
- `fmov.s @(disp,Rn)` y `@-Rm` como carga no existen; `bt`/`bf` alcanzan ±254 bytes (usar
  `bra` con la condición invertida); las tablas de literales van hacia adelante.
- `bsr`/`jsr` pisan `pr`: guardarlo; `jsr` pisa r0-r7 y fr0-fr11; r8-r14 y fr12-fr15 se
  conservan. El marco de pila se libera con inmediatos que quepan (`add #127` + `add #1`).
- Las estructuras con `#pragma pack(1)` (como `XYZ_32` en `types.h`) se desalinean: fijar
  `__attribute__((aligned(4)))` en los campos que el ASM lee como palabra.
- En la build de verify los buffers estáticos grandes le quitan heap al nivel (se cuelga o falla
  `Memory_Alloc`): reservar con `Memory_Alloc` bajo demanda.
- La división entera con signo (`sdiv32`): estimación en float y corrección entera; los
  resultados fuera de rango deben comprobarse en verify.
