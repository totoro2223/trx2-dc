# Tareas pendientes

Solo lo que falta. Lo hecho y lo descartado está en [PROBADO.md](PROBADO.md)
(resumen) y [REGISTRO.md](REGISTRO.md) (detalle): **míralos antes de empezar**
para no repetir una idea que ya falló. Entorno, ciclo de compilación y reglas
con Flycast: [TRASPASO.md](TRASPASO.md).

Al terminar una tarea: anota la fila en PROBADO.md (OK o FALLIDO, con cifra y
build), el detalle en REGISTRO.md, y **bórrala de aquí**. Retira las sondas
`TEMP` (`dc_probe.h` y los `DC_PROBE(...)`) antes de dar un commit por bueno.

Reglas comunes: cambios en código compartido bajo `#if defined(__DREAMCAST__)`;
nada de `build/`, CDIs ni datos del juego en git; no tocar un Flycast abierto
por el usuario; decir siempre "DC build N"; lo que no gane nada medible se
deshace y se anota como FALLIDO.

---

## A. Comprobar lo ya compilado (prioridad alta)

1. **Compilar tras el último pull** y comprobar que los cambios de sombras
   (Soft high, volúmenes con magnificación) conviven con el arreglo del búfer
   de volúmenes de la otra sesión (`M_BuildVolume`, `DC_Shadow_DrawVolumes`).
2. **Ver a ojo los cuatro niveles de sombra** en TR2 y TR3: Soft (high) con
   luces cercanas (deformación de UV en suelos grandes), Hard con
   magnificación, y medir su coste frente a Soft (low).
3. **Ajustar Adaptive 60 FPS** en un nivel pesado (Londres, Bartoli): los
   umbrales 33 ms / 20 ms y los 240 frames de espera están sin probar.
4. **Ver en pantalla** lo de la lista "sin ver" de PROBADO.md (UI reutilizada,
   sombra bajo el agua, sangre y bengala de TR3).

## B. Consola real

5. **Probar en GDEMU y medir.** Flycast no modela las cachés del SH-4 (16 KB de
   datos, 8 KB de instrucciones). Hasta entonces, nada que dependa de la caché
   se puede dar por bueno ni por malo: `M_EmitChunk`, `movca.l`, descarte de
   espaldas por plano, datos calientes/fríos del vértice de sala. Con
   `dc_profile` y `prof_report.py` solo hay una cota optimista.

## C. Optimización del render (lo que de verdad queda)

Reparto de Londres (graphics, ms por frame): pass ~12,7 (estáticos y sprites
3,2, emisión 3,0, objetos 1,6, salas 1,5), stage ~7, Lua ~2,6, lógica ~4,2.

6. **Estáticos fuera de pantalla** (~0,3 ms): prueba previa con esfera antes de
   `Output_CheckBoundsClip` (~10 µs cada uno, 5–10 descartados por sala).
   Riesgo de *popping*.
7. **`UI_RESERVE_ALL`** (0,07–1,0 ms por llamada): alguna raíz de la UI se
   recalcula casi cada frame (señal sondeada que cambia cada tick, o un
   `Tween`). Encontrar cuál.
8. **`TICK` en Lua** (~0,9 ms): sin medir por listener. Medir antes de agrupar
   las lecturas de roles de `input.lua` (era una suposición).
9. **30 reservas por frame** de `Memory_Alloc` (`Catalog_CreateKey`,
   `Memory_DupStr`, `String_Format`).
10. **`Sparks_Control`** (0,2–0,9 ms): hace `Random_GetControl` y luces
    dinámicas; no se puede saltar tal cual.
11. **Emisión** (3,0 ms): es volumen de instrucciones repartido; bajar más pide
    menos caras miradas o vértices ya listos para el PowerVR. Sin pista barata.
12. **Mezcla de matrices de Lara** (`lblend` 0,33 ms): solo vale ~0,16 ms y
    exige saber qué relación guardan las dos pilas (vista y mundo) en DC.
13. **Página de relieve**: el primer bucle (`M_HalfTexel`/`M_PageBlock`) sigue
    siendo por texel.
14. **Retirar las sondas `TEMP`** en bloque (cuestan ~0,5 ms por frame):
    `control.c`, `phase_game.c`, `executor.c`, `lua/events/manager.c`,
    `output/state.c`, `render/output_common.c`, `render/batcher.c`.

## D. Memoria

15. **Datos crudos de sala** (~1,7 MB): `shadows.c` los lee en juego; hay que
    separar lo que necesita antes de poder liberarlos tras `M_ShareVertices`.
16. Tablas de cadenas, animaciones.

## E. Funcionalidad

17. **TR3**: chispas sin interpolación ni giro (`SPARK_F_ROTATE`) y cuadradas;
    salas de color sin el mínimo de sombra ni el parpadeo; guardados en VMU
    (comprobar el nombre `save_tr2_*.dat` en TR3).
18. **Experimental 60**: franja negra arriba a 320×240; comprobar la vuelta a
    640×480; quitar el define temporal `TRX_DC_MMU_NULL_TRAP` (`# DBG`) de
    `src/platform/dreamcast/meson.build`.
19. **Inventario**: 9 → 22 fps, no se ha vuelto a medir tras la arena de Lua.
20. **Audio**: reverb y filtro bajo el agua (programa propio del DSP del AICA).
21. **Deseos del usuario**: gatillo L como modificador (bengala con L+Y), textura
    del cielo, efectos, FMV, armas largas a la espalda (versión reducida de
    `lara_outfits`), Sonido/Controles/Juego en el menú, resto de niveles de TR2
    y luego TR3. Aparcado: pantalla Rendering mínima de DC, mipmaps del mundo,
    subir `vertex_buf_size` de PVR (512 K).

## F. Bug aparcado

22. **Heap corrupto en la demo del Ganges (TR3)**. El usuario cree que es de
    esa demo y no vale la pena seguir. Solo reabrir si aparece en otro nivel.
    Reproducir: `TRX_DC_GAME=tr3 TRX_DC_DEMO=<n>` en `make_disc.sh`.

## G. Stubs que afectan a TR1–TR3

El motor compartido ya decide qué dibujar; lo que falta en Dreamcast es el
destino de dibujo: funciones vacías `DC_STUB()` en
`src/platform/dreamcast/render/stubs.c` (60) y `av_stub.c` (vídeo y
decodificador de audio). Para cada una: implementarla en `render/`, medir y
anotar en PROBADO.md.

**Cómo se hizo la tabla** (23/10/2026): se buscaron los llamadores en
`src/trx/game` y se asignó el juego por la guarda `g_TRVersion` o por el objeto
al que pertenece el efecto. Lo marcado "(probable)" sale del conocimiento del
juego y **no está comprobado en el código ni en pantalla**; confirmarlo al
tomar cada tarea. El código compartido también cubre TR4, que queda fuera del
port. Si TR1 entra en el port (decisión pendiente, ver H), lo único que
cambia es la columna TR1; casi todos los efectos pesados no existen en TR1.

| # | Stub (llamadores) | TR1 | TR2 | TR3 | Prioridad |
|---|---|---|---|---|---|
| 23 | **Letterbox y fundidos** (`Output_Overlay_*Letterbox`, `SetFade`, `DrawBlackRectangle`): `cutseq/playback.c`, `level/common.c`, `flyby_mode.c` | cinemáticas | cinemáticas | cinemáticas y flyby | **Alta**: afecta a las cinemáticas que ya están en el selector |
| 24 | **Instantáneas** (`CaptureSnapshotSlot`, `DrawSnapshotSlot`, `FreeSnapshotSlot`): transiciones de `lua/capi/fade.c` y `ui/draw.c` | sí | sí | sí | Alta: ver si el fondo capturado del inventario ya cubre esto |
| 25 | **Imágenes** (`Output_Overlay_LoadImage`, `DrawImageInBox`): `phase_picture.c`, `ui/draw.c` (pantallas con imagen, cargas) | sí | sí | sí | Media |
| 26 | **Quad de vídeo** (`Output_Quad_*`): `fmv.c`. Va unido a `av_stub.c` (`Video_*`, `AudioDecoder_*`) | FMV | FMV | FMV | Media: es la tarea de FMV de E |
| 27 | **Cielo** (`OutputSource_Sky_StageLayer`, `StageFogGradient`): `output/sky.c` | sin cielo | skybox (probable) | skybox, capa y degradado de niebla | Media: la textura del cielo es un deseo ya anotado en E |
| 28 | **Líneas con efecto** (`PolyFX_StageLineSegment`): goteo (`fx/droplets.c`), láser (`fx/laser.c`), lluvia y nieve (`fx/weather.c`), electrocución de Lara (`lara/electric.c`), dardos (`traps/dart.c`) | dardos (probable) | dardos, nieve del Tíbet (probable), electrocución (probable) | todo lo anterior más láseres de seguridad del Área 51 (`security_laser.c`) y el jefe de la tribu (`tribe_boss.c`) | Media |
| 29 | **Quads y triángulos con efecto** (`StageQuadExt*`, `StageTriExtUV*`): anillos (`fx/ring.c`), estela (`fx/wake.c`), restos (`fx/debris.c`), partículas de agua (`fx/water_particles.c`), murciélagos (`bat_emitter.c`), lluvia | — | estela de lancha (probable) | todos | Media. Las chispas están vacías a propósito (ganó 1,2 ms): si se implementan, volver a medir |
| 30 | **Rayos** (`OutputSource_Lightnings_*`): `traps/lightning_emitter.c`, `output/draw.c` | martillo de Thor (probable) | — | tormentas (probable) | Baja |
| 31 | **Depuración** (`Misc_Stage{Cuboid,Sphere}`, `RoomsDebug_*`): cajas de choque, ascensor, puertas de disparo, `lua/capi/scene.c` | sí | sí | sí | Baja: no son efectos del juego |
| 32 | **Limpieza**: `src/platform/dreamcast/output_stub.c` (963 líneas) no está en `meson.build` y duplica funciones de `render/` | — | — | — | Hacer ya: borrarlo |

Quedan fuera de la tabla los 12 stubs `TRX_GL_*` (contexto OpenGL, temporizador
de GPU, capturas): no se dibujan en Dreamcast ni hacen falta.

Sin cubrir en el código compartido (no hay stub pero tampoco se ha visto en
pantalla): lens flares y bulbos de niebla de TR3, y el efecto de superficie del
agua (`Output_Water_*`). Comprobar en un nivel de TR3 con sol y con agua.

## H. Expansiones con datos de Tomb Raider Remastered

33. **Soportar las expansiones** si la fuente de datos es *Tomb Raider I-III
    Remastered*: Unfinished Business (TR1), Golden Mask (TR2) y The Lost
    Artifact (TR3). Hoy se usan los datos clásicos de Steam (TR2 y TR3).
    Pasos: (a) ver qué trae el remaster y en qué formato están sus niveles y
    textos frente a los clásicos; (b) ver qué entiende ya TRX (`GAME.TXT`,
    `make_disc.sh`, el selector de niveles) y qué habría que añadir;
    (c) cabida en el disco y en memoria (los mapas grandes ya van justos,
    Jungle ~2 MB libres); (d) añadir las expansiones al selector. Decidir
    antes con el usuario si TR1 entra en el port (hoy solo TR2 y TR3).
    El `MAIN.SFX` del remaster ya se lee (builds 269–275).
