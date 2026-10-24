# Pruebas pendientes

Cambios hechos el 23/10/2026 **sin medir ni generar CDI** (solo compilados con
`build.sh minsize`). Cada uno debe verse o medirse antes de darlo por bueno; al
hacerlo, pasar la fila a [PROBADO.md](PROBADO.md) (OK o FALLIDO, con cifra y
build) y borrarla de aquí. Si algo no gana o se ve mal, deshacerlo.

## Resultados de la tanda del 23/10/2026 (DC builds 380-382)

Londres, experimental 60, base build 378 (44,4 fps, con sondas):

| Build | Sondas | fps medio | stage | lógica | pass |
|---|---|---|---|---|---|
| 378 (antes) | sí | 44,4 | 5,4 | 2,9 | 10,7 |
| 380 | sí | 44,6 | 5,5 | 2,9 | 10,7 |
| 381 | no | 45,0 | 5,3 | 2,8 | 10,6 |

- **T14 (sondas apagadas): OK**, +0,4 fps (~0,2 ms), menos de los 0,5 ms que se
  estimaban.
- **T6 (estáticos tras el plano cercano): sin ganancia medible** en Londres
  (`sstat` 0,83 → 0,88 ms, ruido). Sin popping a la vista, pero sin cifra a
  favor: decidir si se deja o se deshace.
- Build 382 (TR3, Jungla, nivel 1): arranca, carga y se ve a Lara y la sala
  sin errores en el log. **No se vio** ninguna cinemática, fundido, letterbox,
  rayo, goteo, lluvia ni chispa; T17 y T23–T30 siguen sin ver.

## TR1 (DC build 385)

`TRX_DC_GAME=tr1 bash tools/dreamcast/make_disc.sh build/dc-minsize '/h/GOG/Tomb Raider I-III Remastered/1/DATA' build/dc-minsize/TRX-tr1.cdi [nivel]`
(motor 1; copia los `.phd` y `MAIN.SFX`; sin `textures.vq`, porque `texvq` solo
entiende el formato de TR2; el selector tiene niveles, cinemáticas y demos de
TR1).

- **Visto (build 385):** el nivel 1 (Cuevas) carga y corre a 30 fps, con Lara,
  nieve y sala bien dibujadas; 24 pistas de música; sin errores nuevos en el
  log.
- **Sin probar:** el selector de TR1 (nombres y orden de niveles, cinemáticas
  `cut1`–`cut4`, demos y benchmark), el resto de niveles (memoria: los de TR1
  son más pequeños, pero sin medir), sonido y música, guardados en VMU, FMV
  (no hay en Dreamcast), texturas en VQ para TR1, rendimiento frente a TR2/TR3
  (el benchmark de TR1 usa la demo de Vilcabamba).

## Pantalla de copyright (DC build 387)

El título de TR2 y TR3 muestra `legal_eu.webp` y luego el vídeo de intro
(`play_fmv`), que en Dreamcast se salta porque no hay decodificador: queda
copyright y después el menú. La imagen no viene en el remaster, así que
`make_disc.sh` la saca del juego clásico (`data/legal.PCX` de TR2 o
`pix/LEGAL.BMP` de TR3) o de `TRX_DC_LEGAL=<archivo>`; sin ella la pantalla
se salta sola.

- **Visto (build 387, TR2):** al elegir "Title screen" en el selector sale el
  aviso de copyright de Core Design a pantalla completa.
- **Sin ver:** que pase al menú del título después (la captura de ese
  momento no se tomó), TR3 (`LEGAL.BMP`), la duración en pantalla, y el vídeo
  de intro, que sigue sin existir.
- Visto de paso, al arrancar directo en el nivel 0 de TR2: un mensaje de Lua
  en pantalla, `attempt to index a nil value (field 'integer index')`, sin
  investigar.

## Vídeos (FMV), DC build 404

Formato propio (`TRXV`, v2), hecho en el PC por `tools/dreamcast/fmvconv.c` desde
los `.FMV` del remaster (flujo PSX: MDEC 320x160 a 15 fps + audio XA):

- Cada fotograma es una **textura VQ de 256x256 RGB565** (libro de 2 KiB de 256
  bloques de 2x2 + 16 KiB de índices) que el PowerVR decodifica solo; la
  película se aprieta a un cuadrado y se estira al dibujar (letterbox 2:1).
  El libro se rehace en cada fotograma, partiendo del anterior.
- Tras cada fotograma van **2940 bytes de audio** (ADPCM Yamaha estéreo,
  44,1 kHz, como la música), así el disco se lee de una vez, en secuencia
  (con el audio en otro archivo la unidad saltaba entre los dos y iba a ~1 fps).
- `render/pvr.c` (`DC_PVR_SetVideoFrame`, `DC_PVR_DrawVideo`), `render/stubs.c`
  (`Output_Quad_*`), `video.c` (`Video_*`), `audio.c` (flujo de película).
- `make_disc.sh` convierte los de `FMV/OG/*.FMV` (o `fmv/*.rpl` del clásico) y los
  deja en `games/<juego>/fmv/<nombre>.fmv`; se guardan en
  `build/dc-minsize/fmv-<juego>-<fuente>`. `TRX_DC_FMV=0` no mete ninguno;
  `TRX_DC_FMV="ancient ms"` solo esos. El encoder tarda ~1 min por cada 1000
  fotogramas.

- **Visto (build 404, TR2, Muralla China, `ancient`):** la intro se reproduce a
  15 fps constantes (121 fotogramas a los 8,0 s, 241 a los 16,0 s), con la imagen
  reconocible (arquero, guerrero, humo), sin errores de DMA.
- **Tamaño:** `ancient` ocupa 93 MB (4370 fotogramas, 21 KB cada uno).
  Todos los de TR2 serían unos 330 MB más; no se ha probado un disco con todos.
- **Sin comprobar:** que se **oiga** el audio y esté en sincronía (no se puede
  escuchar aquí; la imagen sigue un reloj propio que arranca con el audio, no
  su posición); saltar con Start; pausa; TR3 y TR1 (mismo camino, con
  `FMV/OG`); el resto de películas; la lectura en una consola real (el flujo
  son ~320 KB/s, el GD-ROM debería con ello); los fotogramas con degradados
  (VQ de 2x2 con 256 códigos se ve áspero en cielos); la memoria libre durante
  la película (se piden ~0,5 MB).
- La pantalla de copyright y el vídeo "LOGO" (que no existe en el remaster) se
  saltan solos.

## Flujo de arranque con películas (DC builds 425-428)

Como el juego real: copyright, logos, intro y menú. Se cambió el valor por
defecto de `gameplay.intro_fmv_mode` a `INTRO_FMV_LAUNCH` en Dreamcast
(`config/map.def`), así la intro sale al arrancar y no al empezar partida.

- **TR2:** copyright (`legal.PCX` de Steam), logo (`LOGO.RPL` de Steam, por
  `TRX_DC_FMV_EXTRA`), intro `ancient`, menú. **Visto en el log** (build 423, sin
  logo): copyright y luego `ancient` a 15 fps antes del menú.
- **TR1:** copyright `eidos` (`EIDOSPC.PCX` del original, por `TRX_DC_LEGAL`),
  `core.rpl` y `escape.rpl` (por `TRX_DC_FMV_EXTRA`, del TR1 original de Steam,
  carpeta `LARA_RTX`), intro `cafe`, menú. **Visto en el log** (build 428, disco
  de prueba que arranca en el título): copyright 3 s → `core` 165 fotogramas en
  11,0 s → `escape` 100 fotogramas en 4,0 s → `cafe` empezando, todo en orden.
- **TR3:** copyright (`LEGAL.BMP`), `logo`, intro `intr_eng` (de los `.rpl`
  del juego clásico). **Visto en el log** (build 429, disco de prueba que arranca
  en el título): copyright 6,5 s → `logo` 869 fotogramas en 29,0 s (30 fps) →
  intro `intr_eng` empezando (fotograma 2820 a los 94 s), antes del menú.
- Los discos con películas se hacen así (`TRX_DC_FMV_EXTRA` y `TRX_DC_LEGAL` son
  opcionales):

```bash
TRX_DC_GAME=tr1 TRX_DC_LEGAL='<TR1 Steam>/LARA_RTX/DATA/EIDOSPC.PCX' TRX_DC_FMV_EXTRA='<TR1 Steam>/LARA_RTX/FMV' bash tools/dreamcast/make_disc.sh build/dc-minsize '<remaster>/1/DATA' build/dc-minsize/TRX-tr1-fmv.cdi
```

- **Sin comprobar:** el audio de las películas (oído), la imagen del copyright
  de TR1 en pantalla, y los discos de TR3 y TR2 completos tras estos cambios.

## Expansiones (DC builds 430-438)

Unfinished Business (TR1), The Golden Mask (TR2) y The Lost Artifact (TR3), con
los datos del remaster (`DATA/UB`, `DATA/GM`, `DATA/LA`), cada una en un disco
que lleva el juego base y la expansión:

```bash
TRX_DC_GAME=tr2 TRX_DC_MOD=tr2-gm bash tools/dreamcast/make_disc.sh build/dc-minsize '<remaster>/2/DATA' build/dc-minsize/TRX-tr2-gm.cdi
```

(`TRX_DC_MOD` = `tr1-ub` con `TRX_DC_GAME=tr1`, `tr3-la` con `tr3`.) El disco
trae un `MOD.TXT`; `dc_main.c` arranca el motor con `--mod <id>` y el selector
lista los niveles de la expansión (4 de UB, 5 de GM, 6 de LA). Las texturas van
en VQ (los niveles de la expansión entran en `textures.vq`). Discos hechos, sin
películas: `TRX-tr1-ub.cdi` (273 MB, build 436), `TRX-tr2-gm.cdi` (264 MB, 437),
`TRX-tr3-la.cdi` (386 MB, 438).

- **Visto en el log (builds 433-435, discos de prueba con `-l 1`):** el motor
  dice "Mod: tr2-gm" / "tr1-ub" / "tr3-la" y carga el primer nivel de cada una
  desde `games/<mod>/levels/` (`level1.tr2`, `egypt.phd`, `scotland.tr2`; 9-10 s
  de carga); el GM se vio corriendo con sombras y 30+ fps.
- **Sin comprobar:** jugar los niveles (memoria en los grandes), el selector de
  las expansiones en pantalla (se arregló el número de nivel: las expansiones
  cuentan desde 1, sin gimnasio), los finales y créditos, y las imágenes de
  carga (`gm_level*.webp`, `ub_loading*.webp`, `highland.webp`…, que el remaster
  no trae con esos nombres). El título cae al del juego base. Sin películas:
  `TRX_DC_FMV=0` en los discos hechos; con ellas pesarían lo del juego base
  más ~230–330 MB.

## Lara sin torso, cabeza ni armas en Londres (DC builds 440-443)

**Causa:** la lista opaca del PowerVR (160 KiB) se llenaba en la sala del primer
enemigo de Londres con las sombras al máximo (`Soft high`): los polígonos que
no cabían (hasta ~200 por frame) se descartaban, y Lara, que se dibuja la
última, perdía torso, cabeza y armas. El registro lo decía: "N polygons did not
fit the vertex buffers" con OP 160K, mientras la lista translúcida llegaba a 16
KiB de 64 y la de punch-through a 35 de 104.

**Arreglo** (`render/pvr.c`), sin más RAM de la que había (el montón gestionado
es de 12.025 KiB y en Londres llegó a 11.995: quedan 30 KiB libres como mínimo).
Los cuatro búferes (OP/TR/PT/MOD, KiB) eran 160/64/104/72 = 400:
- TR3: 256/24/48/48 = 376 (quita 24 KiB y da 96 KiB más a la opaca).
- TR1 y TR2: 192/32/88/72 = 384.
Con el banco de Londres en TR3 (build 444, graphics, sombras 5): 0 descartados,
pico de la lista opaca 181 de 256 KiB, mínimo libre de RAM igual (30 KiB). Con el banco de Londres (demo 0, sombras 5):

| Build | Modo | Antes: polígonos descartados | Después | fps medio |
|---|---|---|---|---|
| 440 → 442 | graphics, 640x480, 30 fps | rachas de 184–199 por frame | 0 | 28,8 → 29,1 |
| 441 → 443 | experimental 60, 320x240 | rachas de 115–199 por frame | 0 | 41,0 → 41,4 |

(En el 441 las sombras salieron en 3 porque el modo las pisaba; el gancho
`TRX_DC_SHADOW` ya gana al modo, y el 443 va con sombras 5.) La lista opaca
llegaba a 180 KiB como pico con 224 KiB de tamaño. **Visto solo en el registro, no en pantalla:**
falta mirar la sala del primer enemigo en la demo y comprobar que Lara sale
entera. Sigue sin cumplir los 30 fps con sombras al máximo (mediana 32 ms; 35 %
de frames por encima de 35 ms). TR2 usaba más punch-through (~70 KiB en la
Muralla China): 88 KiB le deja ~25 % de margen; comprobar en TR1 y TR2 que
ninguna lista descarta (el aviso "did not fit" del registro).

## Interruptor de 60 fps por interpolación (DC build 447)

Antes solo había un camino a 60 fps: el modo Experimental 60 (que además baja
a 320×240 y pone sombras de volumen), y la opción `rendering.fps` de 30/60
estaba fuera del menú de la consola. Ahora hay un interruptor propio,
`rendering.dc_60fps` ("60 FPS (interpolated)"), en la pestaña Rendering justo
bajo el modo:

- **Apagado de serie**, en el modo inicial (Gráficos, 30 fps).
- **Actúa al momento**, en cualquier modo, sin reiniciar: `DC_Mode_GetFPS` y la
  interpolación lo leen en cada frame. El modo Experimental 60 lo enciende y los
  otros lo apagan; después se puede cambiar a mano. El 60 adaptativo va con él.
- **Visto en el registro** (build 447, Muralla China en modo Gráficos, cambiado
  por la propia fila del menú a la frame 900, Flycast oculto): el contador pasa
  de 30 fps a 50–60 fps al instante, y la fila dice `fps60=true`.
- **Sin comprobar:** con el mando en el menú real, y **cómo se ve en pantalla a
  640×480**: esa resolución es entrelazada en un televisor, y dibujar 60 imágenes
  distintas por segundo puede notarse en movimiento (el modo Experimental 60 usa
  320×240 por eso). Falta ver el apagado desde el menú y lo que cuesta en
  niveles pesados (el 60 adaptativo baja a 30 donde no llega).

## Memoria, guardado y películas (DC builds 456-463)

Informes del usuario: TR3 abortaba al guardar y al cargar, no mostraba las
pirañas ni recibía daño, TR1 fallaba al volver al listado la segunda vez, y el
audio de las películas se cortaba y quedaba desfasado.

- **Limpieza completa al empezar cada nivel** (`level/common.c`): se sueltan
  todos los modelos de objeto construidos y se recoge dos veces la basura de
  Lua. Con el recorrido seguido de niveles (`TRX_DC_LEVELS`, Flycast oculto):
  **TR1 recorre los 15 niveles sin caerse** (antes moría en el 5, Colosseum, por
  falta de memoria) y **TR3 recorre sus niveles hasta Rapids** (nivel 7) sin
  abortar.
- **TR1, nivel 4 (St. Francis' Folly)**: una cara de sala sin esquinas
  (`room 48`, 13 caras) abortaba en el constructor de mallas; ahora se omite con
  un aviso.
- **Guardar y cargar** (`savegame/`): las copias temporales (el árbol JSON, su
  BSON y el comprimido, ~400 KiB; la copia de 119 KiB de los niveles alcanzados)
  van a la región del nivel; antes de guardar o cargar se liberan los modelos de
  objeto que se pueden reconstruir, la basura de Lua y el bloque de las listas
  de polígonos. **Sin probar en el juego**: es la causa de los abortos del
  registro (un `malloc` de 118 KiB sin hueco), pero no se ha guardado ni cargado
  desde el menú tras el arreglo.
- **Volver al listado de niveles** (`dc_main.c`): al terminar el motor, la
  consola arranca el disco otra vez (`arch_reboot`), con toda la memoria limpia;
  antes dependía de la salida de KOS, que funcionó la primera vez y no la
  segunda. Sin probar.
- **Audio de las películas** (`video.c`): al vaciarse la cola de sonido se metía
  silencio y los datos reales quedaban retrasados para siempre. Ahora el sonido
  sigue al reloj: lo que va atrasado más de ~0,25 s se salta. Sin oír.
- **Intro al empezar partida**: la Dreamcast la reproduce al arrancar y otra vez
  al empezar partida nueva (se salta con Start).
- **Pirañas de Jungle** (no se dibujaban ni hacían daño): se supone la misma falta
  de memoria (un modelo se construye al aparecer y falló); sin comprobar. La
  caché de modelos de TR3 baja a 12.000 vértices.

## Muerte, cinemáticas, sombras, efectos y menús (24/10/2026, compilado, sin ver)

Todo compilado con `build.sh minsize`; **ningún disco generado ni ejecutado**.

| # | Cambio | Qué comprobar |
|---|---|---|
| T41 | **TR2: morir** congelaba la imagen y bajaba el framerate a 0. Causa: mis instantáneas (T24) volvían a dibujar la escena entera en una textura y luego la pintaban opaca encima de la animación. `Output_Overlay_CaptureSnapshotSlot` vuelve a dar un hueco sin capturar, y `DrawSnapshotSlot` no dibuja (como el stub original). Las pantallas de fundido a color siguen. | Morir en TR2: se ve la animación de muerte, sin pérdida de fps. Los fundidos entre pantallas ya no cruzan el último fotograma. |
| T42 | **Cinemáticas sin sonido**: las pistas de las cinemáticas (TR2: 2 a 25 y 58) están en la carpeta de idioma del remaster (`TRACKS/EN`) y `make_disc.sh` solo convertía la raíz. Ahora añade `TRACKS/<TRX_DC_LANG, EN por defecto>`. Los discos crecen: más pistas en ADPCM (TR3 tiene 58 en `EN`). | `cut1`..`cut4` de TR2, y las cinemáticas de TR1 y TR3, con voz. Tamaño de cada disco (TR3 ya rozaba los 800 MB). |
| T43 | **Películas desfasadas**: el vídeo seguía un reloj propio y cada corte del audio lo desplazaba. Ahora sigue los bytes de sonido entregados al AICA menos lo que guarda su búfer (24.000 bytes, ~0,55 s): si el sonido se corta, el vídeo espera. Cola de sonido por delante: 16.000 bytes. | Intro de TR2/TR3 entera: sincronía de principio a fin, también tras un corte. Ajustar `M_SOUND_RING_BYTES` si queda adelantado o atrasado. |
| T44 | **Sombras suaves bajo el agua**: ya no se cambian por volúmenes con Lara sumergida (`M_ShadowType`). Sigue sin proyectarse sombra sobre un cuarto bajo el agua mientras Lara está encima (la superficie). | Sombras suaves nadando y sobre la superficie. |
| T45 | **"Saving to VMU..."**: antes de escribir en la VMU (`DC_VMU_StoreSave`) se dibuja un fotograma de carga con el texto abajo ("Do not remove the memory card"), porque la escritura bloquea el juego. Cubre la pantalla con la imagen de carga mientras dura. | Guardar con una VMU puesta: el texto sale y desaparece. |
| T46 | **Etiquetas del mando**: en Dreamcast la entrada por defecto es el mando (`input.backend`), y sus botones se nombran en mayúsculas y texto (`controller.c`): A, B, X, Y, START, L, R, UP/DOWN/LEFT/RIGHT, Z y C (clics de los sticks), D (Back), STICK UP/... | Controles, la ayuda del anillo y los avisos: ya no salen Enter, Espacio ni Esc. Comprobar `START` en el anillo. |
| T34 | **Reflejos de lente del sol** (TR3): `StageQuadExt(Depth)` abre cada billboard (cuatro puntos iguales y `disp`) por los ejes de la cámara; el empuje hacia la cámara de `M_StageTri` queda acotado a tres planos cercanos. | Niveles con sol en TR3: reflejo y fantasmas, y que no los tapen las paredes cuando no deben. |
| T35 | **Nieve** (TR3, Aldwych y Antártida): mismo cambio. | Nieve cayendo, de tamaño razonable. |
| T38 | **Chispas** (`StageSpark`): ahora son cuadriláteros del mundo con el ancho y alto propios (ya no un cuadrado de su media) y giran con `SPARK_F_ROTATE` (humo, explosiones). | Humo, sangre, chispas de disparo, bengalas. Coste con muchas chispas. |
| T36 | **Humo de las pistolas** (TR3): los colores de las chispas de sprite se doblan (escala de 128 como neutro). **Es una hipótesis.** | Disparar las pistolas de TR3: el humo se ve. Si no, ver TAREAS.md I. |
| T37 | **Cáusticas bajo el agua** (TR1 y TR2): `il->caustics` (`water_effect == 1`) suma `sin(fase) × 0x300` al sombreado de cada vértice con una tabla de 32 senos y un hash entero de la posición; las rutas en ensamblador de salas y objetos se saltan bajo el agua (la generic, en C, lo hace). | Nadar en TR1 y TR2: luz que ondula en el fondo y las paredes. Coste de fps bajo el agua. |
| T47 | **Humo de las pistolas (TR3), registro**: `StageSpark` cuenta las chispas de humo vistas, dibujadas y descartadas (fuera de profundidad, demasiado pequeñas, sprite fuera de rango) y lo escribe en el registro cada 240 (`smoke sparks: seen ...`). La hipótesis del color doblado (T36) sigue puesta. | Disparar las pistolas en TR3 y leer el registro: si `seen` sube y `drawn` también, y no se ve, el problema es de dibujo; si `seen` no sube, no se crean. |
| T48 | **Triángulos con textura propia** (restos al romper, partículas de agua, murciélagos): `StageTriExtUV` busca el índice de cada UV (`Output_Textures_FindUVWIndex`), arma un triángulo unidad con esas coordenadas y lo guarda en 16 huecos por clave (el más antiguo se sustituye). Las partículas de agua (billboard) se abren con `disp`. Si las UV no son de ninguna textura, vuelve al triángulo plano. | Romper cristal o un cuarto de restos, cascadas con partículas, murciélagos: ya tienen dibujo, no color liso. Coste de fps con muchos restos. |
| T49 | **Efecto de agua de TR3** en salas (`water_effect >= 2`): los vértices `VERT_MOVE` (olas) y `VERT_GLOW` (brillo) reciben un color añadido que oscila con el tiempo y un desfase por carril de posición, con las mismas tablas de amplitud del escritorio (`M_WaterAdd`). Con ese esquema la ruta en ensamblador de salas se salta. No se mueve la geometría. | Niveles de TR3 con agua: brillo que ondula en la superficie y el fondo. Coste de fps en la sala. |
| T50 | **Ninguna chispa se dibujaba en TR3** (fuego, humo de pistolas, sangre, bengalas, explosiones): `Sparks_Draw` volvía al principio en Dreamcast desde que no había PolyFX (un atajo con "quitar cuando dibuje") y nunca se quitó, así que `StageSpark` no se llamaba nunca. Quitado. Los demás efectos (anillos, estelas, nieve, láseres, rayos, salpicaduras, huellas, restos, murciélagos) sí tienen llamadas y no estaban cortados. | Fuego de trampas y de Lara ardiendo, humo y chispas de pistolas, sangre, bengalas: todos visibles. Coste de fps con mucho fuego (hasta 32 fuegos × 20 chispas, cada una dos veces). |
| T51 | **Aviso al guardar en la VMU**, ahora una caja del estilo del menú (`requester.message`) sobre la misma pantalla, en la parte baja; el guardado se hace dos ticks después (`Passport:save_with_notice`, solo en Dreamcast). Sustituye a la pantalla de carga completa. | Guardar desde el pasaporte: la caja "Saving to VMU... Do not remove the card" aparece sobre el fondo del juego, el guardado se hace y el pasaporte se cierra. |

## Humo, sombras, armas, tres juegos en un disco (24/10/2026, tarde)

Hecho con las pruebas ocultas y las capturas de Flycast que se indican; lo que
dice "sin ver" falta en consola real.

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T52 | **Rendimiento del humo de las pistolas (TR3)**. Causas: cada chispa eran dos instancias de 400 bytes, hacía productos de 64 bits y una división de 64 bits por chispa, `Interpolation_GetWorldRate()` en doble y un registro por el puerto serie cada 240 chispas (unos 20 ms cada uno, el grueso del coste que se veía). Ahora una chispa es **una** instancia (cuadrilátero unidad, `M_GetTriMesh` mitad 3), la vista se pasa a coma flotante una vez por fotograma (`M_RefreshView`) y no hay registro. | Selva de TR3, humo forzado cada 6 fotogramas (`TRX_DC_FIRE=6`, ~80 chispas vivas): `stage` 4,2 → 13,7 ms y `pass` 9,2 → 12,8 ms al principio; **ahora 6,7 y 10,9 ms**, 30 fps. Sin humo: 4,2 y 9,2. | Disparar de verdad en TR3 y mirar los fps. |
| T53 | **Sombra suave: caída más larga** (`M_SOFT_REACH` 1536 → 2048) y **tope de tamaño**: la sombra suave "alta" y los volúmenes crecen como mucho al doble del lanzador (`M_PerspScale` con profundidad mínima la mitad de la distancia a la luz; la longitud del volumen no pasa de la distancia de cada esquina a la luz). | Benchmark de Londres con sombra suave: **28,2 fps antes y después**, mismos ms por etapa: el alargue no cuesta nada medible (Lara es pequeña). Vale la pena. | Ver la sombra en juego, sobre todo con luces cercanas. |
| T54 | **Arma larga en la espalda sin trajes** (`M_SetBackGunFromModel`, `lara/skin/common.c`; giro, escala y pivote en `lara/draw.c`). El disco no lleva `lara_outfits.bin` (5,3 MiB) y los niveles del remaster no traen malla de espalda: Lara salía sin arma. Ahora se pone el modelo del inventario de cada arma larga, girado en diagonal y reducido a 700 unidades. | Capturas en TR2: **escopeta (diagonal) y M16 (horizontal) se ven bien en la espalda**. Lanzagranadas y arpón: no comprobados (el objeto puede no estar en el nivel). Gancho `TRX_DC_GIVE=<LARA_GUN_TYPE>`. | Probar lanzagranadas y arpón; TR1 (escopeta) y TR3 (MP5, cohete); ajustar el sitio si no gusta. |
| T55 | **Aviso de guardado flotante en todos los juegos**: la caja del menú sale en `Passport:save_with_notice`, que es código común (TR1, TR2 y TR3), solo en Dreamcast. | Se comprobó que `book.lua` carga (`SWITCH.TXT` "6 120": `book loaded: table`); no se abrió el pasaporte. | Guardar con una VMU en cada juego. |
| T56 | **Recuperación de caídas** (`M_Recover`, `dc_main.c`): un aserto, una dirección mala o falta de memoria arrancan el disco otra vez en la lista de niveles del mismo juego, en vez de parar la consola. Tres seguidas la paran. | Probado con el aserto de TR3 y la falta de memoria de TR2 de las pruebas de flujo: ver T58. | Ver el efecto en consola real (la nota con el juego queda en RAM alta, `0xACFF0100`). |
| T57 | **Disco con los tres juegos** (`make_disc_all.sh`): `g1`, `g2`, `g3` con sus expansiones (datos del remaster); en el menú principal **Arriba** va al juego anterior y **Abajo** al siguiente, con el aviso en las esquinas derechas; la página de Nueva partida ofrece **Play Expansion**. El cambio reinicia el disco (`dc_games.c`). | En Flycast: arranque en TR1, "next boot: TR2" y arranque en TR2; expansión de TR1 arranca (`--mod tr1-ub`). **Sin películas**: 1,0 GB; con ellas serían ~1,8 GB. | Los avisos en las esquinas no se vieron en pantalla (el menú sale poco tiempo entre demos); que la RAM alta sobreviva al reinicio en consola real (si no, el disco arranca en TR1 y se pierde el juego elegido). |
| T58 | **Pruebas de flujo y de guardado** (Flycast oculto): ver "Resultados" abajo. | | |

### Resultados de las pruebas de flujo (T58)

- **Guardar y cargar en bucle** (`TRX_DC_SAVE=150`, un guardado cada 150
  fotogramas en cinco huecos y una carga después del quinto, con la escritura
  en la VMU de cada uno): TR3 selva, 4 vueltas; TR2 nivel 1, 33 operaciones;
  TR1 nivel 1, 38 operaciones. **Ningún cuelgue, aserto ni caída.**
- **Salir del juego** (`TRX_DC_EXIT`): el disco se reinicia (2 arranques en la
  prueba de TR2 y de TR3), sin caída.
- **Pasar de nivel en nivel sin reiniciar** (`TRX_DC_LEVELS`): TR1, los 8
  niveles bien. **TR2 cae al cargar el nivel 5** tras el 4 (falta de memoria,
  `Memory_Alloc`) y **TR3 falla al cargar el 3** (`inflate failed (-3)` en una
  inyección). Eran caídas que paraban la consola; con T56 reinician el disco.
  Sin arreglar la causa (TAREAS.md, J 60 y 61).

## Memoria entre niveles y opciones en la VMU (24/10/2026, noche)

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T59 | **Fuga de cada nivel, arreglada.** `Walkable_Reset` ponía a cero la tabla de configuración del nivel anterior, que la Dreamcast libera con el resto de las arenas de `GameBuf`: escribía ceros en lo que ocupara ya esa memoria, que era la lista de trozos de otra arena (la de los fotogramas de animación). La lista quedaba cortada y los trozos que le seguían no se liberaban nunca: **~0,9 MB por nivel** en TR2. `Walkable_ResetLevel` ya no toca la tabla vieja en Dreamcast. | Con `TRX_DC_HEAP_CENSUS` y `tools/dreamcast/live_census.py` se vio el dueño (`GameBuf_Alloc` ← `M_ParseFrame`); un contador de trozos hechos y listados (`GameBuf_Check`) dio el punto exacto (entre `Walkable_ResetLevel` y `Rope_Reset`). Lo que la región retiene entre niveles: **de 69 K → 1,5 MB creciendo a 21 K–100 K estable**. | |
| T60 | **Bloques pequeños que sobreviven a un nivel, fuera de la región** (`M_SMALL_MAX`, 16 KiB, `dc_level_heap.c`): quedaban bajo algún bloque grande que el nivel tomó de arriba y, al liberarse éste, partían el hueco del siguiente nivel (el cuarto de TR3 veía 1,9 MB libres seguidos de 6,4). Ahora van al montón de newlib. | **Pasar de nivel en nivel sin reiniciar** (`TRX_DC_LEVELS`, 200 fotogramas en cada uno): **TR1 14 niveles, TR2 17, TR3 12, sin una sola caída**; antes TR2 caía al cargar el 5 y TR3 el 4. La región retiene 7 bloques (40 K) en TR3. | Jugar de verdad de una pasada (la prueba solo carga y deja pasar fotogramas). |
| T61 | **`GameBuf_Alloc` con candado** en Dreamcast (las inyecciones se leen en el grupo de hilos). No era la causa de T59, pero dos hilos sobre una arena podían perder trozos. | | |
| T62 | **Opciones de render en la VMU** (`dc_settings.c`): el modo, 60 fps, cel shading, adaptativo, contador de fps, los dos filtros y el tipo de sombra se guardan en un archivo pequeño (`dcopts.txt`, 2 bloques, `TRX_DCOPTS` en la tarjeta). Se cargan al arrancar el motor (antes del menú principal) y se escriben al **salir del menú principal** (`GF_RunTitle`) y antes de reiniciar el disco, solo si cambiaron. | En Flycast con VMU limpia: arranque 1 cambia una opción y sale → "render options written"; arranque 2: "restored dcopts.txt" y "render options loaded". | Probar en el menú real y en consola real. |

### Aviso: mis pruebas de guardado tocaron la VMU del emulador

Las pruebas de guardado y carga en bucle (`TRX_DC_SAVE`) escribieron en las
ranuras 0 a 4 de TR1, TR2 y TR3 de `G:\Flycast\data\T-TRX00_vmu_save_A1.bin`,
que es la misma tarjeta que usan las partidas del usuario (todos los discos
comparten el identificador `T-TRX00`). Si había partidas ahí, las de esas
ranuras se sobrescribieron. Después se borraron los guardados de prueba que
sobraban (TR2 y TR3, ranuras 3 y 4 de la numeración de Lua; en el archivo
`TRX_TR2_02`, `TRX_TR2_03`, `TRX_TR3_02` y `TRX_TR3_03`) para dejar sitio a las
opciones. La copia previa a la limpieza está junto a la tarjeta:
`T-TRX00_vmu_save_A1.bin.user-backup-0524`. Las pruebas de guardado deben
hacerse en adelante con una tarjeta aparte (mover el archivo antes de probar).

## Recorrido de niveles, guardados y región (24/10/2026, mediodía, DC builds 311–319)

Flycast oculto, `TRX_DC_LEVELS` (300 fotogramas por nivel) y `TRX_DC_SAVE=90`.
Detalle y cifras en REGISTRO.md ("recorrido de niveles y guardados").

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T63 | **Gancho de guardar y cargar con ranuras desde 1** (`executor.c`): `trx.savegame.save/load` cuentan las ranuras desde 1; con la 0, `save` fallaba sin aviso y `load` daba "no saved game in this slot", así que las pruebas T58 nunca cargaron nada. Ahora guarda en 1–5 y carga la 1. | Builds 311–313 (antes del arreglo): TR1 16 niveles y 36 guardados a la VMU sin caída; TR2 cae en Barkhang (12); TR3 12 niveles y Flycast se cierra en Lud's Gate. | Build 320: TR1 16 niveles y 9 cargas reales sin caída. TR2 y TR3 con cargas: discos 324 y 325 hechos, **sin correr**. |
| T64 | **Aviso al no poder copiar a la VMU** (`vmu.c`): `DC_VMU_StoreSave` devolvía `false` en silencio cuando `malloc` del paquete fallaba (montón seco en Temple Ruins de TR3, build 313). | | Ver el aviso en pantalla o en el registro al guardar en un nivel lleno. |
| T65 | **Texturas de TR3 en VQ** (`texvq.c`): solo leía las versiones 0x2D (TR2) y 32 (TR1); los niveles de TR3 (0xFF080038/0xFF180038) quedaban fuera y el disco llevaba 39 páginas (las inyecciones). | Jungle: 30 páginas `vvvvvvvvvvv44444vvvvvvvvvvvvvv`, VRAM libre 2916 → **4126 K** (build 315). | Ver las texturas de TR3 a ojo (VQ pierde algo de detalle). |
| T66 | **Opción `dc_heap_census` de Meson** (el `TRX_DC_HEAP_CENSUS` de la otra sesión no tenía interruptor), volcado y plano de la región al terminar la carga (`level loaded`), tabla de 8192 huecos (3072 se desbordaba: 670 bloques sin rastro), bloques sin rastro con sus primeras palabras, y la línea `game heap after the injections` en todas las compilaciones con watchdog. | `live_census.py` da la composición de un nivel: TR1 level3a ~2,5 MB, TR3 Jungle ~3,6 MB (ver REGISTRO). | |
| T67 | **Piscina de bloques pequeños del nivel** (`dc_level_heap.c`, `M_POOL_SIZE` 256 K en la cima de la región tras un bloque que nunca se libera; los bloques ≤ 256 bytes pedidos durante la carga van allí). Los que sobreviven al nivel (el mapa de solapes de salas, 19–165 B, hecho al final de la carga; el grupo de hilos) partían el hueco del nivel siguiente: TR1 6,6 → 4,4 MB, TR3 6,4 → 2,4 MB. | TR3 Jungle → Temple (build 319): la región retiene 3 bloques (0 K) y el hueco mayor es 5860 K de 5951 K (antes 3382–4618 K). | Con el pico de la carga al 97 % de la región (censo), hasta los bloques del nivel caen en la piscina y el mapa queda 90 K bajo el muro: sin censo hay más margen; medir en la pasada final. Build 320: baja 90 K por carga (6271 → 5448 K); piscina a 384 K en los builds 323–325, sin probar. |
| T68 | **Las matrices del batcher** (`M_GrowVertexBuffers`, `soft`, `oargb`, `env_uv`...) **vuelven al montón de newlib**, donde nacen al arrancar: crecen durante la carga (la transparencia de cada malla se halla dibujándola, al fallar la caché `object_transparency_*` del disco RAM), en el pico, cuando la región solo tiene agujeros, y dos de ellas (41 K) a 4,6 MB partían el hueco de TR3 (builds 316–318). | Build 319: ya no están en la región. | Coste en newlib: hasta ~150 K con mallas de 3000 vértices. |
| T69 | **`run_flycast.ps1`**: la consola de Flycast tiene 120 columnas y partía las líneas largas; el lector la ensancha a 400 y quita los sustitutos sueltos que hacían fallar la escritura del archivo (el log se quedaba viejo a medias). | Logs enteros desde el build 311 (segunda pasada). | |

## Para medir

Las sondas `DC_PROBE` ya **no se compilan por defecto** (costaban ~0,5 ms por
frame). Para el benchmark, compilar con `DC_PROBES=1`:

```bash
DC_PROBES=1 bash tools/dreamcast/build.sh minsize
```

(`build.sh` pasa `-Ddc_probes=true` a meson; sin la variable lo vuelve a
apagar.) Medir antes y después con la misma base, Londres con
`TRX_DC_BENCH=0 TRX_DC_MODE=experimental-60 TRX_DC_GAME=tr3`.

| # | Cambio | Qué comprobar |
|---|---|---|
| T14 | Sondas apagadas por defecto (`dc_probe.h`, `meson.options`, `manager.c`). Los contadores directos `g_DC_ProbeUs[...]++` siguen activos (cuestan poco). | Londres con y sin `DC_PROBES=1`: el frame sin sondas debe bajar ~0,5 ms; el informe sin sondas sale con ceros en las columnas de sondas. |
| T6 | **Estáticos detrás del plano cercano** (`rooms/draw.c`): se descartan antes de `Output_CheckBoundsClip` si su esfera envolvente no pasa del plano cercano. Es conservador (no debería rechazar uno visible). | Ganancia en `sstatics` (esperada ~0,3 ms); recorrer salas con estáticos grandes (columnas, estatuas) girando la cámara: ninguno debe desaparecer al borde de la pantalla. |

## Para ver en pantalla

| # | Cambio | Qué comprobar |
|---|---|---|
| T23 | **Letterbox y fundidos** (`render/stubs.c`: `SetFade`, `SetLetterbox`, `SlideLetterbox`, `UpdateLetterbox`, `DrawLetterbox`, `DrawBlackRectangle`, `HasPending`), con la lógica del escritorio dibujando rectángulos negros por la UI. | Cinemáticas del selector: franjas negras que entran y salen; fundido a negro al empezar y acabar; flyby de TR3; Von Croy de TR2. Que las franjas no tapen la UI de debajo ni queden clavadas al cambiar de nivel. |
| T24 | **Instantáneas**: `CaptureSnapshotSlot`/`DrawSnapshotSlot`/`FreeSnapshotSlot` con **un solo hueco** (la captura compartida); `DrawSnapshotSlot` oscurece, no mezcla con opacidad. | Transiciones de Lua (`trxc.fade.*`): que el fondo capturado salga y no se quede una imagen vieja; con una segunda captura mientras la primera está viva devuelve `nil` (-1). |
| T25 | **Imágenes**: `Output_Overlay_LoadImage` (devuelve si el .565 está en el disco) y `DrawImageInBox` (`DC_PVR_DrawPictureBox`, alfa con la opacidad). | Pantallas de imagen y carga de `phase_picture`; si el disco no trae la imagen la fase termina sola. Comprobar que la caja cae donde el escritorio la pone. |
| T28 | **Líneas con efecto** (`StageLineSegment` en `sprites.c`): tira de dos triángulos planos mirando a la cámara. | Goteo, láseres (TR3 Área 51), lluvia y nieve, electrocución de Lara, dardos. Grosor y color. |
| T29 | **Quads/triángulos con efecto**: `StageQuadExt(Depth)` como quad de sprite sin desplazamientos ni flags; `StageTriExtUV(Depth)` como triángulo plano del color medio. | Anillos y estelas de agua, restos, partículas de agua, murciélagos, destellos de sol de TR3: tamaño correcto (los desplazamientos en pantalla se descartan) y que no se vean como manchas sin textura. |
| T30 | **Rayos** (`OutputSource_Lightnings_StageSegment`): dos tiras sumadas, azul y núcleo blanco. | Martillo de Thor (TR1/probable), tormentas de TR3. |
| T17 | **Chispas de TR3**: ahora se interpolan entre ticks las libres (no ligadas a objeto, efecto ni nodo). Sigue sin giro (`SPARK_F_ROTATE`) y cuadradas. | Sangre, humo, chispas a 60: movimiento fluido, sin saltos al nacer. |
| T32 | `src/platform/dreamcast/output_stub.c` borrado (no estaba en `meson.build`). | Compila (ya comprobado). |

## Flujo normal y títulos (merge con la sesión de TR1)

| # | Cambio | Qué comprobar |
|---|---|---|
| T26a | **Selector**: la primera entrada de cada juego es "Normal flow (logos, title, menu)" (arranca como en producción, sin `-l`). | Que arranque el flujo completo: copyright, logos, intro, menú. |
| T26b | **Título del TR2 clásico** (`make_disc.sh`): `TITLE.PCX` pasa a `pix/title_eu.565` (en TR3, `TITLEUK`) y `credit*.PCX`/`END.PCX` a sus nombres; el aviso legal lo hace el bloque `TRX_DC_LEGAL`. | Ver el menú con la imagen del título y los créditos. |

## Menú de render y selector de sombras (23/10/2026, compilado, sin ver el menú)

| # | Cambio | Qué comprobar |
|---|---|---|
| T33 | **Selector de sombras en Rendering** (`tabs.lua`, `handlers.lua`): `visuals.shadow_type` junto al cel shading, con cuatro valores: Hard, Soft (low), Soft (high) y la original del juego (octágono en TR1/TR2, sprite en TR3). Elegir una **manda sobre el modo**: ya no se fuerza `volume` en Rendimiento y Experimental 60 (`M_ShadowType` solo lo hace bajo el agua). Cambiar de modo repone la sombra del modo (Hard en los de 30 de rendimiento y en el 60, Soft (high) en Gráficos), que se puede cambiar después. | Que las cuatro salgan en el menú, que cada una dibuje lo suyo, y que Soft en Rendimiento no hunda los fps. |
| T34 | **Filas que no hacían nada en la consola, fuera de Rendering**: `rendering.fps` (el ritmo lo manda el modo), `aspect_mode` y `borders` (la pantalla es fija). | Que el menú no las muestre. `rendering.fps` pudo ser lo que "no funcionaba" de los 60. |

Experimental 60 desde el arranque (disco de Venecia con `TRX_DC_MODE=experimental-60`, Flycast oculto, 70 s): 55–60 fps con 320×240, trabajo ~14 ms por frame. El camino por el menú no se probó (el modo oculto no admite teclas).

## Cambio de modo en marcha y TR1 (23/10/2026)

| # | Cambio | Qué comprobar |
|---|---|---|
| T35 | **Cambiar el modo mientras corre**, probado sin teclas con un disco de prueba (`TRX_DC_SWITCH="<modo> <frame>"` en `make_disc.sh`, `SWITCH.TXT`; modo 3 = los pasos de la fila del menú por Lua). Venecia, Flycast oculto: de Gráficos a Experimental 60 sube de 30 a 58–60 fps y la sombra pasa a Hard; por `trx.config.set` y por `rows.change` igual. La resolución 320×240 no cambia hasta reiniciar el nivel. **Error corregido**: `dc_adaptive_fps` nunca se ponía (el `CONFIG_SET` recibía un `int`, que no llega a un ajuste booleano): faltaba el paso a 30 fps en escenas pesadas. | El menú real con el mando: si ahí "no hace nada", falta saber qué pulsas y qué ves; la lógica de debajo funciona. |
| T36 | **TR1 se cae al cargar el nivel 3A** (Lost Valley, `level3a.phd`, datos clásicos de Steam): sin memoria al reservar las muestras (1,4 MB del nivel + ~0,8 MB de inyecciones de sonido, con 12,1 MB ya usados). El nivel 1 sí carga (build 385 de la otra sesión). No es de los cambios de sombras. | Recortar memoria en TR1: inyecciones de sonido que no hacen falta, cargar las muestras antes, o liberar datos de sala. |

## TR1: memoria, texturas VQ y menú (23/10/2026, DC builds 297–310)

| # | Cambio | Qué comprobar |
|---|---|---|
| T37 | **Muestras de TR1 sin bloque en RAM** (`level/sections/audio.c`, DC): cada muestra va del `.phd` a la memoria de sonido al leer los offsets; ya no se reservan 1,4–2,2 MB (el gym) ni los ~0,8 MB de inyecciones que no se usaban. Con `TRX_DC_LEVELS` el gym (nivel 0) y el 3A cargan, y antes abortaban. | Que suenen los efectos de TR1 (no se ha oído). |
| T38 | **Texturas de TR1 en VQ**: `texvq.c` lee los `.phd` (versión 32), pasa las páginas de 8 bits por la paleta como el motor y las añade a `textures.vq` (238 páginas); `make_disc.sh` lo hace también para TR1. Sin VQ la memoria de vídeo bajaba a ~1 MB libres y Flycast se cerraba en el nivel 4. | `texture pages ready (vvv…)` en el log: debe salir `v` en casi todas. |
| T39 | **Menú de TR1**: el fondo `title.webp` sale de `TITLEH.PCX` (y `credits_N` de `CREDN.PCX`); se incluye la música `2.mp3` del título. Antes el disco no llevaba fondo (el gameflow lo llama `title.webp`) ni música (`0 tracks`). | Ver el menú con fondo y música. El TR1 de Steam solo trae las pistas 2 a 9: el resto (la 26, la 57...) sonará mudo hasta tener el juego completo. |
| T40 | **Abierto: TR1 se queda sin memoria hacia el nivel 10 (8A)** al recorrer los niveles seguidos (`TRX_DC_LEVELS`): los bloques que un nivel deja en la región crecen (485 K en el nivel 6, 1,9 MB en el 8A) y parten el hueco libre. Los niveles 0–9 cargaron seguidos. Falta encontrar qué no se libera al descargar un nivel (la región no guarda quién reserva). | Reproducir con `TRX_DC_LEVELS='15 300'` desde el nivel 0. |

## No hecho (queda en TAREAS.md)

Bloque C: 7, 8, 9, 10, 11, 12, 13 (piden medir o investigar). Bloque D: 15 y 16
(exigen separar datos de sala o tocar formatos, riesgoso a ciegas). E17: giro de
chispas, salas de color sin mínimo de sombra ni parpadeo, nombre del guardado
VMU. E20: reverb y filtro bajo el agua (programa del DSP del AICA). G: 31 (dibujos de depuración). El cielo (stubs `Sky_StageLayer`/`StageFogGradient`) no aplica: solo lo llama TR4.
