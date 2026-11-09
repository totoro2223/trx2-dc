# Plan: 60 fps fijos en consola (render nativo de DC + lógica en su hilo)

Preparado el 09/11/2026. Nada empezado ni medido: todas las cifras de este
documento son **estimaciones** hasta la fase 0. Antes de empezar lee
[TRASPASO.md](TRASPASO.md), [PROBADO.md](PROBADO.md) (lo FALLIDO no se repite
sin un dato nuevo), [REA_TR3_PSX.md](REA_TR3_PSX.md) y
[optimizacion render.md](optimizacion%20render.md).

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
