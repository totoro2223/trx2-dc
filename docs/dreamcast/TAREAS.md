# Tareas a realizar

Encargos listos para que los tome otra sesión o agente. Cada uno se entiende
sin la conversación en que surgió. Antes de empezar, lee
[TRASPASO.md](TRASPASO.md) (entorno, ciclo de compilación, reglas con
Flycast) y [REGISTRO.md](REGISTRO.md) (lo ya hecho y medido). La lista
general de pendientes sigue en TRASPASO.md §7.

Al terminar una tarea: anótala en REGISTRO.md con build y cifras, y bórrala
de aquí.

---

## 1. Perfilar y optimizar la lógica de juego y la preparación de la escena

**Estado**: por hacer · **Origen**: sesión del 21/10/2026, tras dejar el
batcher en 17,0 ms.

### Contexto

En Venecia (`venice.tr2`, nivel 3 del selector), Lara quieta al empezar, 30
fps estables en Flycast. Reparto de un fotograma (línea `[frame]`, en 0,1
ms), medido el 22/10/2026 con el código de `a996c8ba7` más los cambios de las
builds 293–294 (ver "Nueva medida de Venecia" en REGISTRO.md; entre
paréntesis, la build 200):

| Parte | ms | Qué es |
|---|---|---|
| `logic` | ~2,4 (3,9) | Desde el final de un fotograma al inicio del siguiente, menos el tiempo dormido: lógica de TR (ítems, Lara, colisiones, cámara), eventos y scripts Lua, interpolación |
| `stage` | ~5,5 (5,0) | Dentro del dibujo, todo lo que no son las pasadas del batcher: recorrer salas por portales, decidir objetos visibles, matrices de Lara y objetos (`Matrix_*`, `Output_CheckBoundsClip`), preparar instancias (`M_Stage`, `MeshBatcher_Stage`), sprites, HUD/UI |
| `pass` | ~11,6 (17,0) | El batcher (ya trabajado a fondo; ver REGISTRO.md) |
| `map` + `submit` | ~1,5 (1,7) | Mapa de la sombra suave y envío de las listas |

Margen actual: ~12,3 ms sobre 33,3 (antes ~5,7). `stage` es ya la mitad de
`pass`. El objetivo es ganar margen para zonas más
cargadas y para el audio que vendrá, sin cambiar el aspecto ni el
comportamiento del juego.

### Qué hacer

1. **Medir antes de tocar.**
   - Perfilador por muestreo: `meson configure build/dc-minsize
     -Ddc_profile=true`, compilar, disco de prueba de Venecia
     (`make_disc.sh ... build/dc-minsize/TRX-prof.cdi 3`),
     `run_flycast.ps1 -Seconds 75`, y
     `py -3 -I tools\dreamcast\prof_report.py <log> build\dc-minsize\TRX.elf
     G:\DreamSDK\opt\toolchains\dc\sh-elf\bin\sh-elf-addr2line.exe 4`.
     **Volver a `-Ddc_profile=false` al acabar.**
   - El perfilador aliasa con los 30 fps; para repartos fiables, poner
     temporizadores temporales (`timer_us_gettime64`) alrededor de los
     bloques grandes y sacarlos con `printf` junto a la línea `[frame]` de
     `M_StatsEndScene` (`render/output_common.c`). Marcar ese código para
     retirarlo y **retirarlo** antes de dar la tarea por hecha.
   - Para saber quién llama a una función de librería (`memcpy`,
     `__floatdisf`, `__divdi3`, `mutex_unlock`...), el truco que funcionó:
     en `M_Profile` (`dc_watchdog.c`) contar el PR solo cuando el PC cae
     dentro de esa función (direcciones con `sh-elf-nm -n TRX.elf`), y leer
     con el modo `pr` de `prof_report.py`.
2. **Partir `logic`**: tiempo de `Game_Control`/fases, ítems activos, Lara,
   cámara, efectos, Lua (`luaV_execute`, `M_FireEvent`,
   `M_CompactListeners`), interpolación. En perfiles anteriores salían
   `Object_Get`, `CatalogTable_TryGet` y `Catalog_IsValidID`: buscar llamadas
   repetidas por frame que se puedan recordar (como se hizo con el traje de
   Lara en `lara/skin/common.c`).
3. **Partir `stage`**: recorrido de salas (`rooms/draw.c`), dibujo de ítems y
   Lara (`lara/draw.c`, `objects/draw.c`), `M_Stage` en
   `output/sources/objects.c` (arma un `MESH_INSTANCE` de ~400 B por malla),
   sprites, `Output_*` de la UI.
4. **Optimizar lo que pese**, con estos patrones que ya dieron resultado:
   - Divisiones y conversiones de 64 bits: el SH-4 no las tiene (libgcc tarda
     ~100 ciclos). Las matrices de TR son `int64_t`; usar
     `Output_MatrixToFloat` (`output/utils.h`) o float.
   - Copias de estructuras grandes por frame (`memcpy` de instancias,
     matrices): evitarlas o reducirlas.
   - Búsquedas repetidas en catálogos o tablas hash por frame: recordar el
     resultado e invalidarlo donde cambie (carga de nivel, etc.).
   - Lua por frame: ver qué eventos/listeners se disparan cada tick.
5. **Comprobar**: compilar (`build.sh minsize`), disco de prueba de Venecia y
   de La Muralla (nivel 1), comparar `logic` y `stage` antes/después, y una
   captura (`flycast_shot.ps1`, tras `flycast_keys.ps1 -FocusOnly`) para ver
   que Lara y la escena se ven igual. Probar también caminando un poco si se
   tocan cosas de Lara o la cámara.

### Reglas

- Cambios en código compartido, protegidos con `#if defined(__DREAMCAST__)`
  salvo que sean equivalentes exactos en PC (y decirlo).
- Nada de `build/`, CDIs, `textures.vq` ni datos del juego en git.
- No tocar un Flycast que el usuario tenga abierto (`Get-Process flycast`).
- Decir siempre "DC build N" de cada disco.
- Lo que no gane nada medible, deshacerlo y anotarlo en REGISTRO.md como
  descartado.

### Hecho cuando

`logic` + `stage` en Venecia bajan de forma medible (o queda documentado por
qué no se puede), sin cambios visibles, con los temporizadores temporales
retirados, las cifras en REGISTRO.md y un commit en la rama `dreamcast`
(prefijo `dreamcast: `).

---

## 2. Batcher: `xform`, `emit` y `cel` en Venecia

**Estado**: puntos 1–7 y 9–16 aplicados en `render/batcher.c` el 21/10/2026,
**compilados pero sin probar** (ni CDI ni Flycast); 17 aplicado también; 8 pendiente. Las
sondas `TEMP` siguen en el árbol. Antes de nada: CDI de Venecia, comparar
`xform`/`emit`/`cel`/`bump` con la tabla de abajo y captura para ver que
todo se ve igual. · **Origen**: sesión del 21/10/2026, medición con sondas
temporales (`dc_probe.h`, DC builds 205–210).

Lo aplicado, en resumen: `M_XVERT` de 32 B con `sargb`/`eargb`/`oargb`/
`env_uv` en arrays aparte; tres bucles (`M_XformRoom` entero con parpadeo y
luces de paso, `M_XformObject` entero con cel, `M_XformGeneric` en float);
`M_Place`/`M_XFORM` con lados, plano cercano y niebla decididos por
instancia; `bind->lit` eliminado (−4 B/vértice de sala); `M_Depth` inline
(`DC_PVR_GetDepth3D`/`DC_PVR_UseDepth` por instancia); `M_Project` directo
al `pvr_vertex_t` con colores explícitos (`M_Colours` por modo de emisión);
cel por copia (`M_KeepCel`/`M_FlushCel`, `M_CEL_RANGE` = 6 WALL_L); inset
horneado en `bind->uv` (`M_InsetRun`) con relleno perezoso
(`tex_dirty`/`M_RefillTextures`, también para las texturas animadas);
caras ordenadas por página (`M_SortByPage`); `M_BumpLight` sin
trigonometría (`M_Atan2` polinómico, `fsrra`).

Revisión posterior (misma sesión, también sin probar): los trozos de sala
reordenan los vértices de la malla por trozo (`M_BuildChunks` →
`M_PlaceVertices`), así que cada trozo es un rango contiguo y no hay lista
(`vlist`/`chunk_verts`/`always` fuera; `batcher->vranges`); la sombra suave
transforma solo esos rangos; `M_EmitIndices` conserva la página entre rangos
(`*page`); los planos de los lados se calculan una vez por instancia
(`M_XFORM.planes`); `M_RefillTextures` sin `Vector_Get` por vértice; la caché
de agua de Lara se invalida al cambiar de nivel; la mezcla de matrices de Lara
redondea con `ftrc` (`M_RoundF`), ortonormaliza solo la salida y cae a
`double` por encima de 2^22; `Item_Control` recalcula los puntos de vida solo
cuando cambian las propiedades (`ObjectProperty_GetGeneration`) o las escalas;
las líneas `[frame]`/`[probe]`/`[luaev]` salen por un hilo propio
(`DC_Watchdog_Print`, `trx-log`), así que ya no cuestan un frame. Pendiente:
`Input_Update` (0,5 ms) sin medir por dentro; no tocar a ciegas.

Medido el 21/10/2026 (Venecia, Flycast, escritorio oculto): logic 2,5 ·
stage 5,0 · pass 16,4 (`xform` 7,2, `emit` 5,0, `cel` 0,5, `bump` 0,56,
`soft` 0,97) · map 1,4. `xform` por partes: salas 3,0 ms (2378 vértices),
objetos 2,7 (2215), genérico 1,1 (554), luz por instancia 0,3, trozos 0,04.
Sin luz ni color, salas 1,07 y objetos 0,8: casi 4 ms son el color. Con la
niebla apagada, `xform` −0,5 ms. Aplicado después: `-O2` solo en
`render/batcher.c` (`#pragma GCC optimize`) y `m_WhiteARGB` (el color de un
vértice blanco sin tinte, por `k`; `bind->white`; TR3 con salas de color
sigue el camino de antes) → pass 14,9, `xform` 6,8, `emit` 4,5, `soft`
0,8–0,9, `bump` 0,49. Lo que queda del color (~1,3 ms) es el sombreado
(`ftrc`, `M_ShadeK`, parpadeo) y la niebla. Copiar `M_XFORM` a una local
en los bucles no ganó nada (descartado).

`emit` por caras (Venecia, por frame): 1045 quads y 721 triángulos mirados;
290 fuera de un lado o en la niebla, 661 de dorso, 42 recortados; 2854
vértices escritos. Sin el trabajo por esquina, `emit` 4,6 → 2,8 ms: 1,8 ms
son las esquinas (0,63 µs cada una) y 2,8 ms el examen de cada cara. Los
quads doblados (una mitad de dorso) son ~3 por frame. La caché de
cabeceras de 64 huecos recompilaba 43 por frame por choques: ahora 256.
Aplicado: caché de 256, `M_EmitFront` (el quad doblado emite su mitad
visible sin volver a mirarla), `M_KeepCel` sin `Vector_Get`. Resultado:
pass 14,6, `emit` 4,37, `cel` 0,40, `xform` 6,7. Queda: `emit` es volumen
de instrucciones por cara y por esquina; bajar más pide menos caras
miradas (dorso por trozo o por normal) o vértices ya listos para el
PowerVR.

### Contexto: lo medido (Venecia, Lara quieta, Flycast, 30 fps)

Frame: logic 4,0 · stage 5,1 · pass 18,8 · map 1,3 · submit 0,3 → ~29,5 ms
de 33,3 (~3,8 ms de margen). Dentro de `pass`:

| Parte | ms | Dato |
|---|---|---|
| `xform` (`M_TransformInstance`) | 8,9 | 5548 vértices → 1,6 µs ≈ 320 ciclos/vértice |
| `emit` (opaco + punch) | 5,2 | 1137 polígonos emitidos |
| `cel` (lado en sombra) | 2,5 | re-recorre las 2215 vértices de Lara/enemigos |
| `soft` (sombra suave) | 1,0 | 76 caras sombreadas |
| `bump` (relieve) | 1,0 | 37 caras → 27 µs/cara, casi todo libm |
| `envp` (reflejos) | 0,04 | |

Caminos de `M_TransformInstance`: `vlit=0 vplain=2215 vgen=3333`, y de los
3333 genéricos **2779 son vértices de sala que salen del camino rápido por
`il.flicker != 0`** (las salas de Venecia tienen `light_mode` ≠ normal):
la caché `bind->lit` no se usa nunca en este nivel.

Dentro de `logic` (3,7 ms): tick Lua 0,94 · `Item_Control` 0,88 ·
`Game_TickEndFrame` 0,77 (3,8 ms cada 5 ticks: `M_AnimateTextures` del
batcher rellena los UV de **todas** las mallas animadas del nivel, se vean
o no) · input 0,50 (C puro) · resto 0,6. Los eventos `BEFORE/AFTER_CONTROL`
no tienen oyentes. `UI_RESERVE` se dispara **9 veces por frame** (1,0 ms,
en `stage`): ver por qué `LUA_UI_ReserveRegions` no lo lanza una sola vez.

La línea `[frame]` por serie cuesta 40–59 ms cada 120 frames: un frame
perdido cada 4 s en toda build con watchdog, **incluida `minsize`**.
Pasarla al hilo del watchdog.

### Plan de `xform`, en este orden (medir `xform` de `[frame]` tras cada paso)

**A. Quitar del bucle lo que es por instancia** (barato, 1–2 ms)

1. Banderas por malla: en `MeshBatcher_AddMesh` calcular `flags_and` y
   `flags_or` de los vértices; si todos tienen las mismas, elegir el bucle
   una vez y quitar las ~10 ramas por vértice (`NO_FOG`, `NO_LIGHTING`,
   `BILLBOARD`, `USE_*_LIGHT`, `REFLECTIVE`, `m_Frame.fog/ortho`, `cel`,
   `env`, `use_lit`, `fill_lit`).
2. Clip de lados por instancia: la esfera ya se prueba contra los 4 planos;
   si cae entera dentro, `clip = 0` sin las 4 comparaciones por vértice.
   Igual con el plano cercano si `eye_z − radio > near_z`.
3. Niebla por instancia: si `|eye| + radio < fog_start`, saltar
   `fipr_magnitude_sqr` + comparación en todo el bucle; si está entera más
   allá, `fog = 1` fijo.

**B. Color en enteros** (1,5–3 ms, sobre todo salas)

4. `M_PackARGB` (~40 instrucciones: 4 clamps, 4 `ftrc`) fuera del camino
   de salas: `shade` es `int16`, `mul = 2 − s/4096` es lineal → 8.8 fijo,
   escalar `color` con `M_ScaleRGB` saturando sin saltos
   (`x | −(x >> 8)`, máscara). Cubre el parpadeo:
   `s' = clamp(shade + (j − 16)·f)` con `j = light_table_idx & 31`, sobre
   un color base cacheado (`color × tint`, sin iluminar).
5. `min_shade` y el clamp de `shade` en entero, una vez por vértice.

**C. Reestructurar el bucle** (0,5–1 ms en Flycast; más en consola)

6. Un bucle por tipo de malla sin `m_Frame` dentro: sala con parpadeo,
   sprites, luz propia; el genérico queda para niebla de color, reflejos y
   ortho.
7. `M_XVERT` de 56 a 32 bytes (una línea de caché): `oargb` con niebla negra
   es un bit → en `clip`; `sargb/eu/ev/eargb` a arrays aparte solo para
   instancias cel/env. No se ve en Flycast (no modela caché).

**D. Transformar menos** (estructural)

8. ~25 % de los vértices transformados son de caras que se tiran (dorso,
   ventana del portal): culling por trozos de sala (TRASPASO §7). Y datos
   caliente/frío: el bucle de sala lee 10 B de cada vértice de 28 B;
   posiciones en un array compacto aparte (toca `mesh_builder` compartido;
   solo rinde en consola).

Estimación: A + B + 6 bajan `xform` de 8,9 a ~3,5–4,5 ms en Flycast. 7 y 8
solo se verán en GDEMU. Ya probado sin ganancia (no repetir): `-O2` en
`batcher.c`, `DC_PVR_Depth3D` inline a secas, fusionar cel con la emisión
opaca (builds 201–202).

### `emit` (5,2 ms) y `cel` (2,5 ms)

Por cada quad emitido: `M_IsQuad`, 4 `clip`, test cel, `M_IsBackFacing` ×2,
`M_GetFaceUVs` (8 conversiones u16→float + 8 `M_Inset` + 2 medias),
`DC_PVR_ReservePoly`, 4× `M_ProjectInto` → `M_Project` (rama ortho,
**llamada no inline a `DC_PVR_Depth3D`** con 3 ramas y `M_UseDepth`, rama de
pasada, 2 `fmul` por `screen_scale`, escribe un `DC_PVR_VERTEX` intermedio
y lo copia al `pvr_vertex_t`), `DC_PVR_CommitPoly`.

9. `DC_PVR_Depth3D` inline con escala precalculada por frame; `m_Depth.top`
   por instancia (el vértice más cercano), no por vértice: ~4000 llamadas
   → 0,5–0,8 ms.
10. `M_Project` directo al `pvr_vertex_t`, sin el intermedio; `screen_scale`
    plegado en `center_x/proj_x` por frame (a 640×480 vale 1,0): ~0,3 ms.
11. Inset bilineal pre-aplicado en `bind->uv` al llenar la textura (los
    vértices TR no se comparten entre caras): quita 8 `M_Inset` + 2 medias
    por quad en **todas** las pasadas: 0,3–0,5 ms.
12. Bucles de emisión por pasada (plana / cel / env / bump) en lugar de las
    ramas `m_Frame.*_pass` por cara y por vértice: ~0,3 ms.
13. Medir cuántas cabeceras escribe `M_Reserve` por frame (cambios de página
    entre caras consecutivas); si son muchas, ordenar las caras por página
    en `MeshBatcher_AddMesh` dentro de cada tramo (tras `M_PairQuads`).
14. **Cel sin recalcular**: el polígono cel de una cara es el mismo
    `pvr_vertex_t` proyectado con `argb = sargb`, `oargb = 0` y en la lista
    PT. Al emitir la cara opaca con cel activo, copiar sus 3–4 vértices a un
    buffer de la instancia (≤ caras × 4 × 32 B; ~13 KB para una malla de
    300 vértices) con el `argb` cambiado, y tras la instancia un solo
    `SetState(PUNCH_SHADE)` + volcado en bloque (store queues). Sin
    `M_GetFaceUVs` ni `M_Project` ni `Depth3D` repetidos: 2,5 → ~0,8 ms.
    Distinto de la fusión probada en 201–202, que recalculaba.
15. Cel por distancia: enemigos lejos de la cámara (pocos píxeles) sin
    pasada cel; umbral en `DC_Mode`.
16. `M_BumpLight` sin trigonometría: `k2 = depth·lz`,
    `k3 = depth·√(1 − lz²)` (identidades exactas de `sin(asin)`,
    `cos(asin)`; la cota pasa a `lz < sin(MIN)`), `atan2f` por aproximación
    polinómica (se cuantiza a 8 bits), los tres `sqrtf` + divisiones por
    `fsrra`: ~0,9 ms.
17. Sombra suave: marcar por vértice en el bucle de `xform` si cae fuera de
    la celda (bits como `clip`) y probar las caras con `and`/`or` de bytes en
    lugar de 6 comparaciones × 3 corners.

### Reglas

Las de la tarea 1. Además: retirar las sondas `TEMP` (`dc_probe.h` y los
`DC_PROBE(...)` en `control.c`, `phase_game.c`, `executor.c`,
`lua/events/manager.c`, `output/state.c`, `render/output_common.c`,
`render/batcher.c`) antes del commit; compilar `minsize` limpio y comparar
`xform`/`pass` antes y después de cada paso. Captura para confirmar que
Venecia y Lara se ven igual.

### Hecho cuando

`xform` y `pass` en Venecia bajan de forma medible paso a paso, con las
cifras en REGISTRO.md, sin cambios visibles, sondas retiradas y commit en
`dreamcast`.

---

## 3. `stage` y `pass`: Lara, UI, trozos de sala y relieve

**Estado**: aplicado el 21/10/2026, **compilado pero sin probar** salvo el
paso A (medido en DC builds 209–212). Antes de nada: CDI de Great Wall y de
Venecia, comparar `stage`/`pass`/`xform` y las sondas `s*`/`l*` con la tabla
de abajo, y captura para ver que todo se ve igual (salas en los bordes de la
pantalla y de los portales, Lara, relieve). Todo debe valer también para TR3.

### Lo medido antes (Lara quieta, Flycast, DC builds 209/210)

| Parte | Great Wall | Venecia |
|---|---|---|
| `stage` | 5,2 | 5,3 |
| Lara (`slara`) | 2,67 | 2,67 |
| ↳ tinte de agua + mezcla de matrices (`lwater`) | 1,12 | 1,11 |
| ↳ mallas (`lmesh`) | 1,19 | 1,20 |
| UI (`sui`; `UI_RESERVE` 9×/frame, 1,0 ms) | 1,62 | 1,63 |
| `pass` | 10,9 | 6,9 |
| `bump` | 1,5 | 0,4 |

### Lo aplicado

- **A. Lara, una sola mezcla por malla** (`lara/draw.c`, DC): la matriz
  mezclada se queda en la pila para la malla en vez de recalcularse en
  `Output_DrawObjectMesh_I` y `Lara_Joints_StashMatrix`. Medido: `lmesh`
  1,19 → 0,28 ms, `slara` 2,67 → 1,67 ms.
- **B. Mezcla de matrices en float** (`matrix.c`, `M_InterpolateFloat`,
  DC): con `-m4-single` cada `double` se emula; la mezcla por cuaterniones
  (~60 µs) pasa a float, con nlerp para giros pequeños y slerp (`acosf`,
  `sinf`) para los grandes; vuelve a los `double` si una entrada no cabe en
  32 bits o el objeto saltó. Vale para todo lo que se dibuja interpolado
  (enemigos incluidos). Esperado: `lwater` ~1,0 → ~0,2 ms.
- **C. Tinte de agua de Lara** (`water/tr123.c`, DC; TR1–3 usan este
  modelo): si ninguna columna de una rejilla 3×3 alrededor de su celda de
  512 tiene agua (`Room_GetWaterHeight`, que cuenta pantanos), ninguna malla
  busca sector; se recalcula al cambiar de celda, de sala o de flipmap.
- **D. UI**: `LUA_EVENT_UI_RESERVE_ALL` (interno) se lanza una vez y
  `trx/ui/regions.lua` recorre las 9 regiones en Lua; `UI_RESERVE` por
  región sigue para scripts de terceros (sin oyentes no cuesta nada). Los
  arneses de tests lanzan también el nuevo evento.
- **E. Trozos de sala** (`batcher.c`, `M_BuildChunks`): las salas de ≥ 256
  vértices se ordenan por celda de 2048 (doblada hasta ≤ 64 trozos) y luego
  por página; cada trozo guarda sus rangos de caras, sus vértices y su
  esfera. Por frame (`M_CullChunks`, solo en el camino `M_XformRoom`) se
  descartan los trozos enteros tras la niebla, ante el plano cercano o
  fuera de un lado (la pantalla o la ventana del portal): ni se transforman
  ni se emiten. Emisión, sombra suave y relieve recorren `batcher->ranges`.
- **F. Relieve**: `M_EmitBump` salía por todas las caras opacas de todas las
  salas; ahora se salta la sala o el trozo cuyo punto más cercano
  (`z_near`) está más allá de `M_BUMP_RANGE`.

Nota aparte: la luz por instancia (`Output_Lights_FillInstanceLight`) no
hace nada en TR1–3, así que no había nada que reutilizar por personaje.

Pendiente, no tocado: el relieve se apaga si un frame baja de 28 fps (p. ej.
al cargar) y no vuelve en la misma escena (`M_UpdateBumpLoad`), lo que
falsea las medidas de `bump` (pasó en la build 211).

### Hecho cuando

Medido en las dos escenas, sin cambios visibles, sondas `TEMP` retiradas,
cifras en REGISTRO.md y commit en `dreamcast`.

## 4. TR3: lo que falta en el render de Dreamcast

Un solo motor y un solo binario (`GAME.TXT` elige juego); las diferencias van
por `g_TRVersion` al preparar la instancia, nunca por vértice. Hecho en las
builds 216–218 (ver REGISTRO.md); queda:

- **Chispas**: sin interpolación entre frames lógicos ni giro
  (`SPARK_F_ROTATE`), y cuadradas (media de ancho y alto). Mirar el humo y el
  fuego de cerca, y a 60 fps.
- **Sangre**: no probada con un enemigo (Jungle empieza sin ninguno a tiro).
- **Luz dinámica de color** (fogonazo, bengala): escrita, no vista en
  captura. Probar con una bengala en un sitio oscuro.
- **Salas TR3**: ahora doblan su color (curva de 128 neutro) sin el mínimo
  de sombra (`lighting_contrast`) ni el parpadeo por `light_table_idx`.
- Memoria: Jungle usa 11,1 MB (~2 MB libres) tras builds 220–222. Siguientes: datos crudos de sala (shadows.c), animaciones, cadenas.
- Guardados: en TR3 la VMU restaura `save_tr2_*.dat`; comprobar el nombre.

## 5. Hacia 16,6 ms por frame: candidatos medidos el 22/10/2026

**Estado**: puntos 2–6, 8 y parte del 9 aplicados el 22/10/2026 (DC builds
260–262 del contador local; ver "Hacia 16,6 ms: candidatos aplicados" en
REGISTRO.md): 21,0 → 19,7 ms. El descarte de caras de espaldas por grupos de
normales (salas por lado, objetos por cono) se probó y se deshizo: sin
ganancia en Flycast (builds 263–266, REGISTRO.md). Queda el 7 (`TICK`, sin
medir por listener). Para seguir bajando: `logic`/`stage` (Lua 1,6, Lara
1,1, ítems 1,1) o medir en consola real, donde la caché cuenta. ·
**Origen**: sesión del 22/10/2026, medición con las
sondas `TEMP` (`dc_probe.h`) sobre `a996c8ba7` + los cambios de las builds
293–294 (ver "Nueva medida de Venecia" en REGISTRO.md). Venecia, Lara quieta,
30 fps: **21,0 ms de trabajo por frame; faltan 4,4 ms para 60 fps**.

### Reparto medido (ms, media de 11 ventanas; log `build/dc-venice-b256.txt`)

| Bloque | ms | Detalle |
|---|---|---|
| Lua | 2,0 | `TICK` 0,90 · `UI_RESERVE_ALL` 0,84 · `UI_PAINT` 0,30 · capas de pintado 0,14 |
| UI en C (`sui` menos Lua) | 0,25 | `UI_Draw` 0,07 ms con 5 operaciones (medido, build 259): la UI cuesta en Lua, no en C |
| Preparar escena | 3,3 | Lara 1,15 (coleta 0,38 · mallas 0,29 · agua 0,20) · ítems 1,08 · estáticos 0,59 · pre 0,35 · portales 0,16 |
| Mapa de sombra suave | 1,2 | siluetas 1,10 (`M_DrawSilhouettes`) |
| Pass: transformación | 4,0 | salas 1,57 (2378 v) · objetos 1,75 (2749 v) · genérico 0,22 |
| Pass: emisión | 4,7 | 2364 caras: 41 % de espaldas, 24 % fuera, 34 % dibujadas |
| Pass: extras | 1,0 | cel 0,36 · relieve 0,49 · sombra 0,19 |
| Pass: caras transparentes | 0,70 | 216 caras, 3,2 µs/cara: una llamada a `M_EmitIndices` por cara (medido, build 259) |
| Pass: resto | ~0,9 | `M_DrawInstance` 11,42 de 11,8: `M_GetBinding`, estado, salidas tempranas y las propias sondas (~40 lecturas de reloj por instancia) |

El perfilador de muestreo (`build/dc-venice-prof-b256.txt`, `build/TRX-prof.elf`)
pone el 64 % de las muestras en `arch_irq_restore` y `genwait_wake_thd_cnt`
(lo que corre con IRQ deshabilitadas se apunta al restaurarlas): fiarse de las
sondas. Entre las funciones del batcher, `M_XformRoom` es la más caliente
(515 muestras, ~27 % de las útiles). `mutex_unlock` tiene 165 (3 %): puede
haber un `malloc`/`free` por frame; localizarlo con el modo `pr`.

### Qué hacer, por ganancia/esfuerzo

1. **Sondas para las dos bolsas sin medir**: hecho el 22/10/2026 (DC build
   259 del contador local, log `build/dc-venice-probes2.txt`, sondas `src*`,
   `p*` y `u*` de `dc_probe.h`). Resultado: la UI en C es 0,07 ms (5
   operaciones por frame); del pass, `src0` (el batcher) son 11,47 de 11,8
   ms, la fuente de la UI 0,02, luces 0,04, `M_PrepareSoft` 0,03,
   `DC_Shadow_DrawVolumes` 0,1–0,25; dentro de `M_DrawInstance` (11,42) lo
   que no cubrían las sondas es el bucle de caras transparentes (0,70) y
   ~0,9 de resto por instancia. No hay más bolsas: lo que falta está en
   `xform` y `emit`. Las sondas nuevas siguen en el árbol como las demás;
   cuestan ~0,5 ms por frame entre todas y se retiran en bloque al acabar.
2. **Caché de reservas de regiones de la UI** (Lua, −0,8 ms).
   `src/lua/trx/ui/regions.lua:248` recorre los 9 roots cada frame
   (`is_visible`, `measure`, `primitive.reserve`) aunque nada cambie. Un
   contador de generación que `Widget:wake()` y la señal `layout`
   incrementen; si no cambió desde el último frame, `LUA_UI_ReserveRegions`
   (`lua/events/ui.c`) no limpia los slots ni dispara el evento. Ojo:
   `Widget:is_visible` lee `visible` por `value_of` sin `follow`; un cambio
   de esa señal también tiene que despertar al widget.
3. **Descarte de trozos de sala por cono de normales** (batcher, −0,8 ms).
   `M_CullChunks` solo prueba el frustum; el 41 % de las caras se descarta de
   espaldas (`M_IsBackFacing`) después de transformar e iluminar sus vértices.
   Un cono de normales por trozo, precalculado al construir la malla,
   descartaría paredes enteras vistas por detrás antes de `M_XformRoom`.
4. **Siluetas de la sombra suave a mitad de frecuencia** (−0,55 ms).
   `DC_Shadow_RenderMaps` redibuja la silueta de Lara cada frame; a 15 Hz un
   borrón suave no se nota. Alternativa: reutilizar el mapa mientras las
   matrices de los casters y la luz no cambien.
5. **Caché de iluminación por ítem** (−0,3 ms). `Output_CalculateObjectLighting`
   → `M_CalculateLight` (`output/lights/tr12.c`) recorre las luces de la sala
   y hace `Math_GetVectorAngles` por ítem y frame; recordar el resultado
   mientras pos/rot/sala y luces dinámicas no cambien, como el traje de Lara.
6. **Coleta** (−0,2 ms). `M_WeldBridge` (`lara/hair.c:716`) resincroniza la
   geometría de 3 mallas cada frame (`Output_DispatchObjectMeshGeometry` →
   `M_ResyncGeometry`). Soldar solo cuando los anclajes se muevan más de un
   umbral.
7. **`TICK` en Lua** (−0,3 ms). `tick:set` (`signal.lua:475`) propaga a ~10
   señales `polled` más el bucle de roles de `input.lua:812` (una llamada a C
   por rol seguido). Agrupar las lecturas de roles en una sola llamada a C.
8. **Caras transparentes en tandas** (−0,3 ms). `M_DrawInstance` llama a
   `M_EmitIndices` una vez por cara transparente (216 por frame, 3,2 µs cada
   una frente a ~2 de las opacas); juntar las caras consecutivas de la misma
   página en una llamada, o guardarlas ya concatenadas al construir la malla.
9. **Pequeños**: `exp2f` ×3 por luz y por instancia de sala en
   `M_PrepareRoomLights` (precalcular al subir las luces); `M_GetBinding` por
   uthash en cada instancia (guardar el binding en la instancia al preparar);
   retirar las sondas (~0,5 ms).

Suma de 2–9: ~3,8 ms → ~17,2 ms. Y esto es con Lara quieta: en juego la
lógica y los ítems suben. Para 16,6 estables hace falta además recortar
`xform` (4,0) o `emit` (4,7) de forma estructural: el cono de normales por
trozo (punto 3) es lo que ataca los dos a la vez.

### Reglas y "hecho cuando"

Las de la tarea 1: medir antes y después en Venecia y La Muralla, captura
para ver que nada cambia, código compartido bajo `__DREAMCAST__` salvo
equivalentes exactos, sondas `TEMP` retiradas, cifras en REGISTRO.md y commit
en `dreamcast`. Al regenerar el disco de medida, recordar que
`build/dc-build-number` de esta máquina (257) no es el del registro.

### Cómo retomar el ensamblador del transform (estado al 2026-10-22)

1. Compilar con la verificación: `meson configure build/dc-minsize -Ddc_emit_verify=true`, luego `build.sh minsize`; disco de Venecia (`make_disc.sh ... 3`), Flycast oculto 75 s; buscar `[xform-verify]` (y `[emit-verify]`) en el log: deben decir `0 bad` (la línea se parte a 120 caracteres: leer la siguiente). Después poner `-Ddc_emit_verify=false` y medir `troom`/`pass` con `probes.py`.
2. `xform_sh4.S`: la ruta de vértices NO blancos (macro `CHANNEL`, etiqueta `tinted`) ya ensambla y está activa (2026-10-22): el fallo era un `	` de la macro convertido en tabulador (`	int`); el parámetro se llama ahora `off`, y la puerta `fast` de `M_XformRoom` ya no exige `white`. Verificada en Venecia: `[xform-verify]` 0 bad con 1047 vértices no blancos (sonda `rnw`).
3. `M_XformObject` hecho (2026-10-22): `render/xform_obj_sh4.S` (`dc_xform_object`; TR1/TR2, sin reflejo ni `vary`, blanco o tintado, gemelos y cel). Verificado en Venecia con `dc_emit_verify` (0 bad, vértices y cel). `tplain` 1,72 → 1,48 ms (pass 94 → 92). Copia de vértices compartidos hecha con `fschg` + `fmov.d` (4 pares de 64 bits en lugar de 8 palabras; verificada, 0 bad): troom ~1,09/1,17 → 1,04/1,12 ms, tplain ~1,48 → 1,41 ms. Ruta TR3 hecha (2026-10-22): salas (factor `tr3_k` en lugar del sombreado) y objetos (luz de objeto por canal, `tr3col`); verificada en la Jungla de TR3 con `dc_emit_verify` (0 bad). TR3: troom 1,29 → 0,94 ms, tplain 1,79 → 1,57 ms, pass 104 → 98. Pendiente: quitar a granel las sondas `TEMP` de `dc_probe.h` y `batcher.c`.
3b. Siguientes pistas medidas con la demo de Londres (22/10/2026): Lua 3 ms por frame (UI_PAINT, TICK, UI_RESERVE_ALL siguen caros en la demo: ver si la UI de la demo puede cachearse), estáticos con luz propia (~2400 vértices, 1,1 µs cada uno), emisión de caras 3,0 ms.
4. Falta medir en la consola real (hoy solo Flycast).
