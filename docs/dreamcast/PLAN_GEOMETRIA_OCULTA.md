# Plan: geometría que no se ve (objetos tapados, trozos más finos, relieve)

Preparado el 30/10/2026 a partir de las pruebas de oclusión de salas (DC builds
841-855, `PROBADO.md` y `REGISTRO.md` del 30/10/2026). Antes de empezar lee
[TRASPASO.md](TRASPASO.md) y **`PROBADO.md`**: lo FALLIDO no se repite sin un
dato nuevo. Nada de esto está empezado ni medido.

## Qué se sabe (medido en Flycast, TR3 y TR2)

- La oclusión por rayos en la rejilla de sectores ya funciona para **trozos de
  sala** (chunks) y **portales**: en una vista abierta de TR3 (13 salas) ningún
  portal sale oculto porque todos se ven de verdad, y se tapan 6 de 8 trozos.
  Más oclusión de salas tiene poco que quitar.
- Del coste por frame en TR3 (vista pegada a una pared): `stage` ~7 ms desde
  que la sombra sprite se cambió por el octágono; `pass` de 10 ms (modo
  Original) a 21-34 ms (con relieve y sombra suave). En el pase, el relieve
  (`bump`) costó 7,5 ms y la sombra suave 3,1 ms.
- Vértices transformados por frame: salas 5.500-7.600 y **objetos 2.300-2.800
  (~27 %)**. Los objetos (estáticos, ítems, enemigos) no pasan por la prueba de
  rayos: se dibujan siempre que su esfera esté en pantalla.
- Las salas se parten en una rejilla de celdas de 2.048 unidades con un tope de
  128 trozos (`M_MAX_CHUNKS`, `M_BuildChunks`): una sala grande dobla la celda
  hasta caber, así que puede tener trozos de 4.096 u 8.192.
- Se dibuja trozo a trozo adrede (la caché de datos de la SH4 no cabe una sala
  entera): **no** juntar los trozos por página entre salas.
- FALLIDO que no hay que repetir: descartar caras de espaldas por conos
  (`eback` 979 → 767 y el pase 11,6 → 11,9 ms, builds 263-266).

## Tareas, por orden de valor

### G1. Objetos tapados por paredes

Qué: antes de dibujar un estático o un ítem, cinco rayos del ojo a su caja,
contra las paredes de la sala de la cámara y las de la sala del objeto, igual
que en `M_ChunkHidden`. Si todos chocan, no se dibuja.

Dónde: **a nivel de ítem y de estático, no de malla** (un enemigo son ~15
mallas; probar cada una multiplicaría el coste). En `trx/game/rooms/draw.c`,
`M_DrawRoomItem` (ítems) y el bucle de estáticos de `M_DrawSingleRoom`, con la
posición y `draw_bounds` del objeto. `OutputSource_Rooms_RayBlocked(room, eye,
to)` ya existe (`rooms.c`, `batcher.c`); el ojo es `g_ViewPos`.

Cuidado:
- Nunca a Lara. No a objetos a menos de ~2 bloques del ojo.
- Margen: la caja se agranda (los enemigos se salen de `draw_bounds`).
- `Item_ControlDraw` y `bind->drawn` siguen como están: lo que cambia es solo
  si se llama a `obj->draw_func` / `Object_DrawMesh`.
- Cajas de depuración (`DC_PROBES=1`, d-pad arriba): un color nuevo (naranja)
  para los objetos tapados, para ver los falsos.

Medir: `vobj` y `pass` con y sin; una sala con muros y estáticos detrás.
Criterio: `vobj` baja, el coste de los rayos (~250 por frame con 50 objetos)
menor que lo ahorrado, y ningún enemigo ni ítem que desaparezca a la vista.

### G2. Trozos más finos en salas grandes

Qué: subir `M_MAX_CHUNKS` (128 → 256 o 512) para que las salas grandes no
dupliquen la celda tanto. Revisar todo lo que dimensiona con él: `shows[]` y
`z_near[]` de `M_CullChunks`, `used[]` y `run_cells[2][..][2]` de
`M_BuildChunks`, `m_ChunkState[]`, y la capacidad de `batcher->vranges` y
`vrange_chunk`.

Antes: sacar con `LOG_INFO` (solo con sondas) el tamaño de celda y el número de
trozos de cada sala grande al cargar, para saber si hay algo que ganar. Si las
celdas ya son de 2.048 en casi todas, no hacer nada.

Medir: `ctest`/`chide`, `xform` y `pass`; el coste por trozo sube, así que hay
que ver que no se pierde lo ganado.

### G3. Relieve y sombra suave bajo carga

Qué: el relieve (`bump`, 7,5 ms junto a una pared) ya se apaga bajo 15 fps
(`M_BUMP_MIN_FPS`). Subir ese umbral (24-30) o poner un tope de caras de
relieve por frame (`M_BUMP_QUEUE` es la cola). Lo mismo con la sombra suave si
sigue cara.

Medir: fps en la vista pegada a la pared con y sin; ver si el apagado
intermitente se nota (parpadeo del relieve).

### G4. Caras diminutas (solo si G1-G3 no bastan)

Descartar caras de menos de ~1 píxel² en pantalla; el área ya sale del cálculo
de caras de espaldas (`M_IsBackFacing`). Riesgo: huecos de un píxel entre caras
vecinas. **Medir antes con una sonda** cuántas caras caen bajo el umbral: por
la fila FALLIDA de arriba, quitar caras baratas de rechazar puede no mover el
pase.

## Antes de dar nada por bueno

- Fila en `PROBADO.md` (OK/FALLIDO, cifra y build) y detalle en `REGISTRO.md`.
- Retirar las sondas TEMP de la oclusión: `ctest`, `chide`, `croom`, `qtest`,
  `qhide` (`dc_probe.h`, `output_common.c`) y los `g_DC_ProbeUs[...]++ // TEMP`
  de `batcher.c` y `rooms/draw.c`.
- Probar TR1, TR2 y TR3 (rayos y alturas de pared distintos en cada uno) con un
  disco sin vídeos; comprobar con las cajas que no hay rojos ni naranjas sobre
  lo que se ve.
