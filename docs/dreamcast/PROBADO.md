# Lo probado: qué funcionó y qué no

Una fila por idea, con el último resultado. **Antes de proponer una
optimización o un arreglo, búscalo aquí**: si está en FALLIDO, no se repite
sin un dato nuevo (otro hardware, otro nivel). Los detalles y las cifras
completas, en [REGISTRO.md](REGISTRO.md) (cronológico). Lo que falta por hacer,
en [TAREAS.md](TAREAS.md).

Actualizado el 23/10/2026. Todo se midió en **Flycast** (no modela las cachés
del SH-4) y nada se ha probado en consola real. "Venecia" = `venice.tr2`,
"Londres" = demo de Aldwych de TR3 (benchmark). Estados:

- **OK**: medido o visto, queda en el árbol.
- **FALLIDO**: probado, sin ganancia o con efecto malo; deshecho.
- **SIN VER**: compilado, no comprobado a ojo ni medido.

## Estado actual de rendimiento

| Escena | Antes | Ahora | Fuente |
|---|---|---|---|
| Venecia (Lara quieta, 30 fps) | 27,6 ms por frame (build 200) | ~18,1 ms (pass 9,9) tras el ensamblador | REGISTRO, builds 267–275 |
| Londres, Graphics | 26,9 fps | 28,7 fps (pass 14,3 → 12,7; frames > 35 ms: 44 → 34 %) | builds 351–374 |
| Londres, Performance | 27,5 fps | 29,3 fps | ídem |
| Londres, Experimental 60 | 39,0 fps | 44,7 fps (frames > 35 ms: 15 → 5 %) | builds 351–375 |

## Render y rendimiento

| Idea | Resultado | Cifra | Build |
|---|---|---|---|
| Texturas VQ en el disco | OK | VRAM libre 1121 → 4420 KiB | 160–170 |
| Niebla/distancia originales (12–20 casillas) | OK | más vértices (2358 → 5548) pero fiel al original | 161 |
| Espera al PVR al final del frame | OK | wait 2,3 → 0 ms | 177 |
| Luces de sala descartadas por instancia, `fsrra`, bucle rápido de salas | OK | xform 12,0 → 7,3 ms | 172–177 |
| Cabeceras PVR en caché, polígonos directo al búfer, `plain_object` | OK | pass 17,0 ms | 186–200 |
| `-O2` solo en `batcher.c`/`pvr.c` (por librería o fichero) | FALLIDO | neto nulo | ~200 |
| `-O2` en todo el proyecto | OK | lógica 4,8 → 4,2 ms; texto +0,2 MB | 351–374 |
| `DC_PVR_Depth3D` inline a secas | FALLIDO | sin ganancia | ~201 |
| Fusionar el cel con la emisión opaca (recalculando) | FALLIDO | 17,0 → 17,0 | 201–202 |
| Cel por copia de vértices (`M_KeepCel`) | OK | cel 2,5 → 0,4 ms | 2026-10-21 |
| Vértice de sala compartido se transforma una vez | OK | salas 1,8 → 1,0 ms | 233–236 |
| Esquinas gemelas en objetos + reflejos por la ruta rápida | OK | objetos 4,1 → 1,7 ms (Jungle) | 237–241 |
| `movca.l` al escribir cada vértice | FALLIDO (no medible) | Flycast no emula la caché; probar en consola | 237–241 |
| Copiar `M_XFORM` a locales en los bucles | FALLIDO | troom 1,55 → 1,53 | ~267 |
| Ensamblador SH-4: `dc_emit_run` (emisión) | OK | emit 4,6 → 2,9 ms, 0 diferencias en 12 M de índices | 267–275 |
| Ensamblador SH-4: transform de salas y objetos, TR1/2/3 | OK | troom 1,69 → 1,34; tplain 1,72 → 1,48 | 267–275 |
| Ensamblador SH-4: estáticos y sprites (`dc_xform_own`) | OK | pass 14,3 → 13,5 (Londres) | 314–342 |
| Copia de vértice compartido con `fschg`+`fmov.d` | OK (poco) | −0,06 ms | 267–275 |
| Descarte de caras de espaldas por conos/lados (salas y objetos) | FALLIDO | `eback` 979 → 767 pero `tplain` y `emit` no bajan; pass 11,6 → 11,9 | 263–266 |
| Descarte de espaldas por plano para ahorrar lecturas de caché | NO HECHO | solo rinde en consola; reabrir con GDEMU | — |
| Trozos de sala por celda (frustum/portal/niebla) | OK | recorta xform/emit fuera de pantalla | 209–212 |
| Emitir cada trozo de sala nada más transformarlo (`M_EmitChunk`) | OK sin medida | Flycast no ve la caché; comprobar en consola | 260–262 |
| Caras transparentes en una sola tanda | FALLIDO | 0,70 → 0,69 ms; el coste es la emisión | 260–262 |
| Caché de luz por ítem | FALLIDO (no compensa) | 0,14 ms en total | — |
| `M_GetBinding` guardado en la instancia | FALLIDO (no compensa) | ~50 µs por frame | — |
| `exp2f` → `M_Exp2` | OK (ruido) | dentro del ruido | 260–262 |
| Coleta: soldadura guardada mientras los anclajes no se muevan | OK (poco) | `lhair` 0,38 → 0,33 | 260–262 |
| Lara: una mezcla de matrices por malla | OK | `lmesh` 1,19 → 0,28; slara 2,67 → 1,67 | 209–212 |
| Lara: mezcla en float (nlerp/slerp) | OK | `lwater` ~1,0 → ~0,2 | 209–212 |
| Lara: nlerp hasta 50° en vez de 5° | FALLIDO | slara 1,263 → 1,253 | 376 |
| Lara: mezclar solo la matriz de mundo y multiplicar por la vista | FALLIDO | la matriz de vista no es `g_ViewMatrix`×mundo en DC; `lblend` subió a 0,39 | 378 |
| Tinte de agua de Lara por rejilla 3×3 | OK | agua 0,09 ms | 209–212 |
| Relieve: saltar sala/trozo lejos (`M_BUMP_RANGE`) | OK | bump 1,5 → 0,4 (Venecia) | 209–212 |
| Relieve: página con polinomio de atan | OK | quita tirones de 105–150 ms | 351–374 |
| Relieve adaptativo (se apaga bajo 28 fps) | OK | 3 ciclos apagado/encendido por demo, no oscila | 351–374 |
| Sombra suave a 15 Hz | FALLIDO (visual) | no gustó; vuelve a cada frame | 314–342 |
| Siluetas de sombra como tiras de cuatro | OK | 1,22 → 1,10 ms | 251–253 |
| Sombra de volumen: búfer de 24 → 36 triángulos | OK | era un desbordamiento (caía la demo en Rendimiento) | 314–342 |
| Entrada: apagar teclado/táctil en DC | OK | `inp` 0,50 → 0,17 | 251–253 |
| `Sparks_Draw` vacío en DC (el staging es un stub) | OK | sfx 1,6 → 0,4 ms | 351–374 |
| `String_IsEmpty` sin regex PCRE2 | OK | 53 → 30 `malloc` por frame, pass −0,4 | 351–374 |
| Luz propia con niebla por tabla (`xform_own`) | FALLIDO | tgen 2,52 → 2,50 | 375 |
| Lua: señales con versión (`_ver`/`_seen`) | OK | `UI_PAINT` 2,5 → 0,33 ms | 270–275 |
| Lua: reservas de regiones guardadas (`root._reserve`) | OK | sui 1,53 → 0,79 ms | 260–262 |
| Lua: recolector generacional | OK | −0,22 ms por frame | 351–374 |
| Lua: `track`/`derive` sin tablas, barra repite quads | OK | Lua 2,96 → 2,62 ms | 343–350 |
| UI reutilizada en frames fraccionarios sin tick | OK, SIN VER a ojo | sui 1,47 → 1,10 | 375 |
| Widget que escucha su `visible` | FALLIDO | 0,39 → 0,84 ms | 242–249 |
| Lógica de juego a 15 Hz | FALLIDO | el usuario lo rechazó (ticks fijos de 1/30 s) | — |
| 60 fps adaptativo (baja a 30 si el trabajo > 33 ms, vuelve < 20 ms, 240 frames de espera) | SIN VER | compilado; umbrales sin ajustar en un nivel pesado | 281 local |
| Opción "Adaptive 60 FPS" en el menú, ligada al modo gráfico | SIN VER | compilado | 281 local |

## Sombras

| Nivel | Estado |
|---|---|
| Hard (volúmenes modificadores, con magnificación desde la luz) | SIN VER; el coste de construir volúmenes sube sgflush ~1 ms |
| Soft (low), el anterior | OK |
| Soft (high), con magnificación por distancia y caída (`M_PerspScale`) | SIN VER; riesgo: el PVR interpola UV afín y las caras grandes (suelos de 1–4 bloques) pueden deformarse con luces cercanas |
| Blob original | OK |
| Sombra suave: 2 luces, más oscura cerca, sin estirarse en paredes | SIN VER a ojo |
| Sin sombra de Lara en salas bajo el agua | SIN VER a ojo |

Por defecto, el modo Gráficos usa Soft (high).

## Memoria

| Idea | Resultado | Cifra |
|---|---|---|
| Salas con vértices compartidos (`M_ShareVertices`) | OK | Jungle −813 K |
| `SECTOR` estrecho en DC (100 → 56 B) | OK | Jungle −488 K |
| Tope de la caché de objetos | OK (red de seguridad) | no actúa hoy |
| Liberar vértices/caras crudos de sala | FALLIDO | `shadows.c` los lee en juego |
| `-fshort-enums` | FALLIDO | la config escribe enums por `int32_t *` |
| Cargas repetidas (8 seguidas) | OK | mayor hueco 5,2–6,5 MB (builds 285–292) |
| Jungle hoy | OK | 11,1 MB usados, ~2 MB libres |
| Piscina de bloques pequeños del nivel en la cima de la región (`M_POOL_SIZE`, 256 K, bloques ≤ 256 B de la carga) | OK | TR3 Jungle → Temple: hueco mayor 3382–4618 K → 5860 K de 5951 K; TR1 partía 6,6 → 4,4 MB por el mapa de solapes de salas (build 319) |
| Matrices del batcher (`M_GrowVertexBuffers`) en el montón, no en la región | OK | dos de 41 K a 4,6 MB partían el hueco de TR3; crecen en el pico de la carga (build 319) |
| Bloques pequeños de la carga a la cabeza con los de larga vida (sin piscina) | FALLIDO | el superviviente queda 0,8–1,1 MB bajo la cima, debajo de los pequeños transitorios (build 316) |
| Texturas de TR3 en VQ (`texvq.c` leía solo TR1 y TR2) | OK | 30 páginas en VQ; VRAM libre 2916 → 4126 K (build 315) |
| Censo de la región (`-Ddc_heap_census=true`, `live_census.py`) | OK | TR1 level3a ~2,5 MB cargado, TR3 Jungle ~3,6 MB; pico de carga 5,4 / 6,0 MB (inyecciones y conversión de texturas conviven con el nivel) |
| Montón de newlib fuera de la región | Medido | TR2 al cargar Barkhang: región 6592 K + Lua 2304 K + **~2,2 MB en bloques < 24 K** de larga vida; `brk` a 124 K del final de la RAM |

## Sonido, entrada y juego

| Idea | Resultado |
|---|---|
| Efectos ADPCM en el AICA, 50 canales | OK (builds 269–275) |
| MAIN.SFX del remaster (tabla previa de 740 B) | OK |
| Música en streams (ADPCM estéreo, hilo `trx-music`) | OK, corregido en 276–284 (canales 0–7 libres, búfer de 32 K, pila de 32 K); 100 s sin fallo |
| Reverb y filtro bajo el agua | NO HECHO (necesita programa propio del DSP) |
| Atajo L+R+X+Y (fin de nivel) | OK |
| Cinemáticas: pulsación guardada durante la carga se saltaba | OK, corregido |
| Demos y benchmark de Londres, selector en tres páginas | OK; informes en `docs/dreamcast/benchmarks/` |
| Coleta de Lara con el objeto `O_LARA_HAIR` del nivel | OK (TR2 y TR3) |
| Partida cargada sin pistolas ni coleta | OK, corregido (build 255) |
| TR3: Lara completa, luz de color, chispas, una sombra | OK (215–218); sangre y luz de bengala SIN VER |

## Errores abiertos

| Error | Estado |
|---|---|
| Heap corrupto en la demo del Ganges (TR3) | Abierto, aparcado por el usuario. Sale solo en esa demo; se evita desactivando el heap por nivel |
| Cuelgue al morir/reiniciar el nivel | Muy mejorado (8 cargas seguidas bien, builds 285–292). Falta confirmar el caso exacto de morir |
| Experimental 60: franja negra arriba a 320×240 | Sin confirmar |

## Cosas hechas que el usuario no ha visto en pantalla

Reflejos de pistolas con intensidad baja, sombra del cel según la luz, relieve
más intenso y su apagado por fps, culling por portal y borde (buscar caras que
faltan en los bordes), sombras Soft (high) y Hard nuevas, Adaptive 60 FPS,
sombra bajo el agua, UI reutilizada entre ticks.

## Visto en juego (24/10/2026)

- **Huellas de TR3**: salen bien con el mapa de esquinas 0, 1, 3 en los
  triángulos de sprite (`sprites.c`, `M_GetTriMesh`). Las salpicaduras y ondas
  usan el mismo camino, sin comprobar aparte.

- **Humo forzado en TR3** (selva, Flycast oculto, humo cada 6 fotogramas): con
  una instancia por chispa y la vista en coma flotante, `stage` +2,5 ms y
  `pass` +1,7 ms sobre 30 fps (antes +9,5 y +3,6). Era sobre todo el registro
  por el puerto serie.
- **Alargar la caída de la sombra suave** (1536 → 2048): 28,2 fps antes y
  después en el benchmark de Londres.
- **Escopeta y M16 en la espalda** (TR2, nivel 5, captura de Flycast): se ven.
- **Guardar y cargar en bucle** en TR3 (selva): cuatro vueltas de cinco
  guardados y una carga, sin cuelgue ni caída (el registro tiene el ruido
  "Out of memory. Requested sbrk_base" del asignador, que no es un fallo).
- **Salir del juego y volver a entrar** (`TRX_DC_EXIT`): el reinicio del disco
  funciona.
- **Disco de tres juegos**: arranca en TR1 y cambia a TR2 y a la expansión de
  TR1 con el reinicio.

- **Pasar de nivel en nivel sin reiniciar** (Flycast oculto, 200 fotogramas por
  nivel, tras arreglar la fuga de `Walkable_Reset` y sacar de la región los
  bloques pequeños): TR1 14 niveles, TR2 17, TR3 12, sin caídas.
- **Opciones de render en la VMU**: se escriben al salir del menú principal y
  se restauran al arrancar (VMU limpia de Flycast).

- **Recorrido de niveles con guardados** (mediodía, 300 fotogramas por nivel,
  `TRX_DC_SAVE=90`, DC builds 311–313): **TR1 los 16 niveles** y 36 guardados
  a la VMU sin caída; **TR2 cae al cargar Barkhang (12)**: la región se llena
  (85 464 reservas desviadas al montón) y el guardado siguiente no encuentra
  memoria (`Memory_Alloc` aserto); **TR3 carga 12 niveles** (hasta Lud's Gate)
  y Flycast se cierra solo sin nada en el registro. Las cargas de partida del
  gancho no cargaban nada (ranura 0, inexistente): corregido. **Build 320**:
  TR1 16 niveles y 9 cargas reales sin caída; hueco mayor 6271 → 5448 K (baja
  ~90 K por carga; piscina ampliada a 384 K sin probar, builds 323–325).
- **La copia a la VMU falla en silencio** cuando el montón está seco (TR3
  Temple Ruins, guardados 7 y 11 de la prueba): ahora avisa.
- **La VMU de 128 K** se llena con ~12 partidas (9–26 bloques cada una).
