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

En Venecia (`venice.tr2`, nivel 3 del selector), DC build 200, Lara quieta al
empezar, 30 fps estables en Flycast. Reparto de un fotograma (línea
`[frame]`, en 0,1 ms):

| Parte | ms | Qué es |
|---|---|---|
| `logic` | ~3,9 | Desde el final de un fotograma al inicio del siguiente, menos el tiempo dormido: lógica de TR (ítems, Lara, colisiones, cámara), eventos y scripts Lua, interpolación |
| `stage` | ~5,0 | Dentro del dibujo, todo lo que no son las pasadas del batcher: recorrer salas por portales, decidir objetos visibles, matrices de Lara y objetos (`Matrix_*`, `Output_CheckBoundsClip`), preparar instancias (`M_Stage`, `MeshBatcher_Stage`), sprites, HUD/UI |
| `pass` | ~17,0 | El batcher (ya trabajado a fondo; ver REGISTRO.md) |

Margen actual: ~5,7 ms sobre 33,3. El objetivo es ganar margen para zonas más
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
