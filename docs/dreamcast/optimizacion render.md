# Optimización del renderer

Propuesta: ordenar el renderer actual, no escribir uno nuevo.

## Por qué no uno nuevo

- `src/platform/dreamcast/render/batcher.c` tiene 10.813 líneas y 147 funciones estáticas, pero ya tiene unos 14.000 líneas de asm SH-4 con su C de referencia y sus verificaciones `DC_EMIT_VERIFY`. Un renderer nuevo tira todo eso.
- Habría que volver a validar niebla, distancia de dibujo, sombras, agua y los tres juegos. La niebla debe quedar igual que en el original.
- Casi todo el trabajo reciente de rendimiento está en el asm (`room_sh4.S`, `emit_sh4.S` y los demás). El desorden de `batcher.c` no es hoy el cuello de botella.

## Plan

1. **Partir `batcher.c`** en archivos por responsabilidad, moviendo código sin cambiarlo, y comprobar con la verificación que el resultado es idéntico bit a bit. Cortes propuestos:
   - proyección y transformación
   - habitaciones
   - objetos
   - partículas y clima
   - sombras
   - agua
   - emisión y cola al PVR
2. **Unificar el patrón de cada pieza de asm** (llamada, comparación con la C, interruptor `ASMOFF`). Hoy `g_DC_AsmOff` y `g_DC_AsmOff2` son máscaras de bits sueltas, y cada pieza repite el mismo bloque de verificación a mano.
3. **Optimizar solo con perfil.** Medir en el build de sondas (`build/dc-minsize-probes`) dónde se va el tiempo del frame antes de tocar nada.

## Riesgo

Solo el paso 1 es seguro y mecánico. Los pasos 2 y 3 cambian código y van aparte.

## Estado (09/11/2026)

Se midió primero (perfilador `DC_PROFILE=1` del 05/11, sondas, benchmarks) y
se siguió con el plan de asm. Paso 1 sin hacer: `batcher.c` sigue entero
(10.823 líneas); el trabajo fue a los `*_sh4.S`. Paso 2 sin hacer: ya son dos
máscaras (`TRX_DC_ASMOFF` y `TRX_DC_ASMOFF2`) y más de 30 bloques de
verificación repetidos. Paso 3 hecho como método.

## Investigación del 09/11/2026: qué queda de los motores de DC (SA2) y del PSX

Sin código ni medidas: revisión del código y estimaciones. Referencia: el
benchmark `benchmarks/20261108023419_build750_tr1.txt` (TR1, sombra suave,
18,7 fps): `xform` 22,8 ms para ~5.200 vértices, unos 4 µs por vértice (PSX
0,23; un motor de DC hecho a medida, 0,3-0,5). El cuello no es escribir
vértices (las store queues no ganaron en consola), sino el trabajo por vértice
antes de escribirlo.

### Ya hecho (no repetir)

| Técnica | Dónde |
|---|---|
| UV de cada esquina calculada al cargar (16 bits) | `bind->uv`, `batcher.c` |
| Cabeceras de la PVR en caché, polígonos directo a la lista | PROBADO, builds 186-200 |
| Agua animada por tabla | `m_WaterTab`, `water_asm` (`batcher.c`) |
| Trozos de sala con esfera y oclusión por rayos | `M_BuildChunks`, `M_ChunkHidden` |
| Partículas como el PSX (0x73560): una proyección del centro, un quad de 4 vértices, sin malla ni matriz | `M_DrawParticles`, `particles_sh4.S`; PROBADO fila de Antártida (18,5 → 28 fps) |
| Lluvia y nieve sin mallas y en asm | `weather_sh4.S`; PROBADO |
| Store queues y `pref` | Opciones `dc_sq_vertices`, `dc_xform_pref`: sin ganancia en consola |
| Tabla de blanco (color por factor) | `xform_sh4.S` |

### A medias

- **Vértices de sala compartidos**: `M_ShareVertices` los junta al cargar,
  pero el bucle de `xform` recorre cada esquina, mira `vref`/`share_slot` y
  copia (1.709 de 2.688 esquinas en el benchmark). Es la tarea 83 de TAREAS:
  recorrer solo los únicos y que las caras lean `xverts[vref[esquina]]`
  (`LOAD_CORNER` de `emit_sh4.S` y las lecturas de `xv[]` en C).
- **Luz estática**: el sombreado viene horneado en los datos, pero el color
  final (tinte, parpadeo, sombreado mínimo, doble de TR3) se vuelve a armar
  cada frame. Guardar el ARGB final de los vértices sin luz dinámica, agua ni
  parpadeo (`rplain` + `rnw`, ~930 por frame en el benchmark) y rehacerlo solo
  cuando cambie el tinte o la luz.

### Sin empezar (de más a menos valor estimado)

1. **Media caché de datos como RAM** (`CCR.ORA`, 8 KB en `0x7C000000`): el
   equivalente del scratchpad `0x1f800000` del PSX, donde guarda la lista de
   luces del trozo. Ahí irían la matriz, las luces del trozo, las tablas y los
   `xverts` del trozo en curso (127 × 32 B ≈ 4 KB). Coste: la caché normal
   baja a 8 KB. Solo se mide en consola.
2. **Vértices de intensidad de la PVR** (TR1/TR2): el color en la cabecera y
   un brillo por vértice; el sombreado de TR1/TR2 es de un canal. Quita el
   armado del ARGB y el tinte por vértice. No sirve para el color de TR3.
3. **Niebla de la PVR por tabla** (`pvr_fog_table_custom`), 1-2 días. El
   mundo va en la capa base con z = 1/w (`DC_PVR_Depth3D`), así que la tabla
   vale; overlay e interfaz, sin niebla. Riesgos: pasa de distancia radial a
   profundidad (como el PSX, pero cambia la imagen: **decide el usuario**,
   la niebla no se toca); un solo color por frame (la niebla de color por el
   tinte de la instancia, `batcher.c` ~3012, se queda en la CPU); revisar las
   pasadas de sombra suave, cel y relieve. Ganancia estimada 0,3-1 ms.
4. **Descarte por la PVR** (hoy `PVR_CULLING_NONE`, `pvr.c`): en trozos
   enteros dentro de pantalla y lejos del plano cercano, emitir sin mirar
   caras y dejar que la PVR descarte las de espaldas. No es la fila FALLIDA de
   los conos (aquello añadía trabajo a la CPU); se escriben más vértices, hay
   que medir.
5. **Polígonos sprite de la PVR** para partículas: 64 B por quad en vez de
   128, pero sin color por vértice y con el color en la cabecera; el fundido
   de niebla cambia el color por partícula, así que casi cada una lleva su
   cabecera (96 B). Ganancia marginal sobre lo ya hecho.
6. **`.DCL`** (fase B de PLAN_REFACTOR_RAM.md): semanas, riesgo medio-alto,
   sin ganancia directa de fps (sirve para la carga y la RAM). Vía barata a
   comprobar: volcar la región del nivel (`dc_level_heap.c`) ya cargada y
   leerla en la misma dirección, si todo el nivel vive en ella.

Descartado: tiras largas (cada cara de TR usa su propio tile, las esquinas no
comparten UV), caché de vértices entre frames (la cámara casi siempre se
mueve), imágenes de salas lejanas, transformar salas un frame sí y otro no,
y acortar la niebla.
