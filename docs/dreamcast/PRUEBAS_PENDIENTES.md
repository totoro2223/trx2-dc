# Pruebas pendientes

Cambios compilados que aún no se han visto ni medido. Al comprobar uno, pasar
la fila a [PROBADO.md](PROBADO.md) (OK o FALLIDO, con cifra y build) y
borrarla de aquí. Si algo no gana o se ve mal, deshacerlo. Desde el 02/11/2026
la build por defecto no lleva Lua (`dc_lua=false`): lo que dice `tabs.lua`,
`book.lua` o `rows.change` vale solo para `DC_LUA=1`.

Revisado contra el código el 09/11/2026: lo ya probado pasó a PROBADO.md
("Revisión del 09/11/2026") y lo que el código ya no tiene se borró.

Sin anotar aún (sin commit, sin probar): página Sonido con "Soundtrack"
Original / TR Suite (`audio.dc_suite_music`, `TRX_DC_SUITE`), fila "Screen"
4:3 / 16:9 anamórfico (ver T34b) y los ganchos `TRX_DC_CUTSCENE` y
`TRX_DC_WIDE`.

## Resultados de la tanda del 23/10/2026 (DC builds 380-382)

Londres, experimental 60, base build 378 (44,4 fps, con sondas):

| Build | Sondas | fps medio | stage | lógica | pass |
|---|---|---|---|---|---|
| 378 (antes) | sí | 44,4 | 5,4 | 2,9 | 10,7 |
| 380 | sí | 44,6 | 5,5 | 2,9 | 10,7 |
| 381 | no | 45,0 | 5,3 | 2,8 | 10,6 |

- **T6 (estáticos tras el plano cercano): sin ganancia medible** en Londres
  (`sstat` 0,83 → 0,88 ms, ruido). Sin popping a la vista, pero sin cifra a
  favor: decidir si se deja o se deshace.
- Build 382 (TR3, Jungla, nivel 1): arranca, carga y se ve a Lara y la sala
  sin errores en el log. T23, T25, T28 y T30 siguen sin ver.

## TR1 (DC build 385)

`TRX_DC_GAME=tr1 bash tools/dreamcast/make_disc.sh build/dc-minsize '/h/GOG/Tomb Raider I-III Remastered/1/DATA' build/dc-minsize/TRX-tr1.cdi [nivel]`
(motor 1; copia los `.phd` y `MAIN.SFX`; `texvq` también lee TR1, ver T38b).

- **Visto:** nivel 1 a 30 fps (build 385); recorrido de los 16 niveles y
  guardados (PROBADO); películas de arranque (más abajo).
- **Sin probar:** el selector de TR1 en pantalla (cinemáticas `cut1`–`cut4`,
  demos), el sonido de efectos y música oído, y el rendimiento frente a
  TR2/TR3 fuera de la demo de Vilcabamba.

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

- **Sin comprobar:** discos completos de TR2 y TR3 con todas las películas
  tras los cambios de MPEG-1 (el sonido ya se oyó sincronizado en consola,
  PROBADO).

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
- **Sin comprobar:** jugar los niveles (memoria en los grandes), los finales y créditos, y las imágenes de
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

**Arreglo** (`render/pvr.c`, desde `f0a49048e`): las cuatro listas comparten un
fondo de 400 KiB (384 en TR3) que se reparte al empezar cada frame según el
pico que pidió cada una (`M_Repartition`, margen 1,25, baja en ~140 frames);
mínimos OP/TR/PT/MOD 48/24/8/16 KiB. Antes (builds 442-444) era un reparto
fijo. Con el banco de Londres en TR3 (build 444, graphics, sombras 5): 0 descartados,
pico de la lista opaca 181 de 256 KiB, mínimo libre de RAM igual (30 KiB). Con el banco de Londres (demo 0, sombras 5):

| Build | Modo | Antes: polígonos descartados | Después | fps medio |
|---|---|---|---|---|
| 440 → 442 | graphics, 640x480, 30 fps | rachas de 184–199 por frame | 0 | 28,8 → 29,1 |
| 441 → 443 | experimental 60, 320x240 | rachas de 115–199 por frame | 0 | 41,0 → 41,4 |

**Visto solo en el registro** (builds 442-444, aún con el reparto fijo): 0
descartados. Falta ver en pantalla a Lara entera en la sala del primer enemigo
de Londres, y comprobar en TR1 y TR2 que no sale "did not fit" en el registro.

## Frame rate 30/60 (`rendering.dc_fps`)

Fila "Frame rate" de Gráficos (`nolua_settings.c`): 30 o 60 interpolado;
Experimental 60 pone 60 y los demás 30, cambiable a mano, actúa sin
reiniciar. El 60 adaptativo se quitó (`5c6e7f989`).

- **Visto en el registro** (build 447, con Lua): 30 → 50–60 fps al cambiarlo.
- **Sin comprobar:** cómo se ve 60 fps a 640×480 en un televisor
  (entrelazado), y su coste en niveles pesados.

## Memoria, guardado y películas (DC builds 456-463)

(La limpieza al empezar nivel, el guardado, la vuelta al listado y el audio
de las películas ya están probados o sustituidos: ver PROBADO.)

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
| T44 | **Sombras suaves bajo el agua**: ya no se cambian por volúmenes con Lara sumergida (`M_ShadowType`). Sigue sin proyectarse sombra sobre un cuarto bajo el agua mientras Lara está encima (la superficie). | Sombras suaves nadando y sobre la superficie. |
| T46 | **Etiquetas del mando**: en Dreamcast la entrada por defecto es el mando (`input.backend`), y sus botones se nombran en mayúsculas y texto (`controller.c`): A, B, X, Y, START, L, R, UP/DOWN/LEFT/RIGHT, Z y C (clics de los sticks), D (Back), STICK UP/... | Controles, la ayuda del anillo y los avisos: ya no salen Enter, Espacio ni Esc. Comprobar `START` en el anillo. |
| T34a | **Reflejos de lente del sol** (TR3): `StageQuadExt(Depth)` abre cada billboard (cuatro puntos iguales y `disp`) por los ejes de la cámara; el empuje hacia la cámara de `M_StageTri` queda acotado a tres planos cercanos. | Niveles con sol en TR3: reflejo y fantasmas, y que no los tapen las paredes cuando no deben. |
| T36a | **Humo de las pistolas** (TR3): los colores de las chispas de sprite se doblan (escala de 128 como neutro). **Es una hipótesis.** | Disparar las pistolas de TR3: el humo se ve. Si no, el registro de T47 ya no existe (se quitó en `e71212391`): añadir una sonda. |
| T37a | **Cáusticas bajo el agua** (TR1 y TR2): tabla de 32 enteros y hash de la posición dentro del ensamblador de salas/objetos (PROBADO, DC_EMIT_VERIFY 0 distintos). | Verlas a ojo nadando en TR1 y TR2, y el coste en consola. |
| T48 | **Triángulos con textura propia** (restos, partículas de agua, murciélagos): `M_StageTriExtUV` abre el billboard con `disp` y pasa las UV tal cual a `M_StageQuadPts` (desde `1296e0884`; antes, huecos por clave con `Output_Textures_FindUVWIndex`). | Romper cristal o un cuarto de restos, cascadas con partículas, murciélagos: ya tienen dibujo, no color liso. Coste de fps con muchos restos. |

## Humo, sombras, armas, tres juegos en un disco (24/10/2026, tarde)

Hecho con las pruebas ocultas y las capturas de Flycast que se indican; lo que
dice "sin ver" falta en consola real.

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T53 | **Sombra suave: alcance** `M_SOFT_REACH` 3072 y tope de tres veces el lanzador (`424d7d9ae`); celdas de 64 texels, 96 en Soft (high) (`5c6e7f989`). | Con 2048: 28,2 fps antes y después (PROBADO). | Ver la sombra en juego con luces cercanas. |
| T54 | **Arma larga en la espalda sin trajes** (`M_SetBackGunFromModel`, `lara/skin/common.c`; giro, escala y pivote en `lara/draw.c`). El disco no lleva `lara_outfits.bin` (5,3 MiB) y los niveles del remaster no traen malla de espalda: Lara salía sin arma. Ahora se pone el modelo del inventario de cada arma larga, girado en diagonal y reducido a 700 unidades. | Capturas en TR2: **escopeta (diagonal) y M16 (horizontal) se ven bien en la espalda**. Lanzagranadas y arpón: no comprobados (el objeto puede no estar en el nivel). Gancho `TRX_DC_GIVE=<LARA_GUN_TYPE>`. | Probar lanzagranadas y arpón; TR1 (escopeta) y TR3 (MP5, cohete); ajustar el sitio si no gusta. |
| T55 | **Aviso de guardado en todos los juegos**: caja del pasaporte en C (`nolua_passport.c`, `M_SaveWithNotice`) para TR1, TR2 y TR3. | Visto en TR2 (PROBADO). | Guardar en TR1 y TR3. |
| T56 | **Recuperación de caídas** (`M_Recover`, `dc_main.c`): un aserto, una dirección mala o falta de memoria arrancan el disco otra vez en la lista de niveles del mismo juego, en vez de parar la consola. Tres seguidas la paran. | Probado con el aserto de TR3 y la falta de memoria de TR2 de las pruebas de flujo: ver T58. | Ver el efecto en consola real (la nota con el juego queda en RAM alta, `0xACFF0100`). |

## Memoria entre niveles y opciones en la VMU (24/10/2026, noche)

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T62 | **Opciones de render en la VMU** (`dc_settings.c`): el modo, 60 fps, cel shading, adaptativo, contador de fps, los dos filtros y el tipo de sombra se guardan en un archivo pequeño (`trdctriopt`, que desde `5c6e7f989` guarda también el último juego jugado). Se cargan al arrancar el motor (antes del menú principal) y se escriben al **salir del menú principal** (`GF_RunTitle`) y antes de reiniciar el disco, solo si cambiaron. | En Flycast con VMU limpia: arranque 1 cambia una opción y sale → "render options written"; arranque 2: "restored dcopts.txt" y "render options loaded". | Probar en el menú real y en consola real. |
| T71 | **Avisos de los juegos del disco, expansión en el selector y resultado del guardado.** Cajas flotantes en las esquinas del título del juego ("Press Up/Down to go to TRn", estilo de la caja del guardado); el selector previo al motor lista los niveles de la expansión (página 4) y L/R cambian de juego; el pasaporte muestra "Saved to the VMU" o el error antes de cerrar. Ya no reinicia la consola (T70). | Flycast: título de TR1 y TR2 con sus cajas; Abajo pasa de TR1 a TR2; selector: página de la expansión y "Return to Egypt" arranca `egypt.phd` en la expansión; guardado de TR2 con el resultado en la caja. | Consola real; L/R del selector con el mando; TR3 y expansiones de TR2/TR3 desde el selector; guardar con tarjeta llena o sin tarjeta. |
| T70 | **Cambio de juego en el mismo proceso** (`dc_main.c`, `dc_games.c`): Arriba/Abajo del título, L/R y la expansión del selector previo al motor, y el "Play Expansion" del pasaporte no reinician la consola: el motor termina su hilo (`Shell_Terminate` → `DC_Main_EndGame`, ya no `exit()`, que en TR2 hacía caer Flycast con "SH4 exception when blocked") y `__wrap_main` vuelve a lanzar el juego elegido (`DC_Games_ApplyPending`, la raíz `g_DC_Root` cambia, la pila del hilo se libera). Hubo que limpiar lo que dejaba una ejecución para la siguiente: `m_IsExiting` y el estado del ejecutor de fases (`PhaseExecutor_Reset`), los interruptores de los discos de prueba y sus contadores, `TRX_DIR`. | Flycast aparte (`G:/FlycastTest`), oculto, disco de tres juegos sin vídeos: TR1→TR2→TR3 y TR2↔TR3 seis veces seguidas (7 minutos), la región retiene 287 K (TR2) y 299 K (TR3) en cada vuelta sin crecer; la expansión de TR1 desde el selector arranca `egypt.phd`. | Consola real; las 287–299 K retenidas por juego (algo de la primera ejecución que no se libera); jugar de verdad tras cambiar de juego (solo se probaron los títulos); un fallo grave (`Shell_Terminate(1)`) ahora devuelve 1 al arranque en vez de llamar a `exit()`. |

## Recorrido de niveles, guardados y región (24/10/2026, mediodía, DC builds 311–319)

Flycast oculto, `TRX_DC_LEVELS` (300 fotogramas por nivel) y `TRX_DC_SAVE=90`.
Detalle y cifras en REGISTRO.md ("recorrido de niveles y guardados").

| # | Cambio | Qué se midió o comprobó | Qué falta |
|---|---|---|---|
| T64 | **Aviso al no poder copiar a la VMU** (`vmu.c`): `DC_VMU_StoreSave` devolvía `false` en silencio cuando `malloc` del paquete fallaba (montón seco en Temple Ruins de TR3, build 313). | | Ver el aviso en pantalla o en el registro al guardar en un nivel lleno. |
| T65 | **Texturas de TR3 en VQ** (`texvq.c`): solo leía las versiones 0x2D (TR2) y 32 (TR1); los niveles de TR3 (0xFF080038/0xFF180038) quedaban fuera y el disco llevaba 39 páginas (las inyecciones). | Jungle: 30 páginas `vvvvvvvvvvv44444vvvvvvvvvvvvvv`, VRAM libre 2916 → **4126 K** (build 315). | Ver las texturas de TR3 a ojo (VQ pierde algo de detalle). |

## Para medir

Las sondas `DC_PROBE` ya **no se compilan por defecto** (costaban ~0,5 ms por
frame). Para el benchmark, compilar con `DC_PROBES=1`:

```bash
DC_PROBES=1 bash tools/dreamcast/build.sh minsize
```

(`build.sh` pasa `-Ddc_probes=true` y compila en `build/dc-minsize-probes`; el
disco se hace con `make_disc.sh build/dc-minsize-probes …`.) Medir antes y después con la misma base, Londres con
`TRX_DC_BENCH=0 TRX_DC_MODE=experimental-60 TRX_DC_GAME=tr3`.

| # | Cambio | Qué comprobar |
|---|---|---|
| T6 | **Estáticos detrás del plano cercano** (`rooms/draw.c`): se descartan antes de `Output_CheckBoundsClip` si su esfera envolvente no pasa del plano cercano. Es conservador (no debería rechazar uno visible). | Ganancia en `sstatics` (esperada ~0,3 ms); recorrer salas con estáticos grandes (columnas, estatuas) girando la cámara: ninguno debe desaparecer al borde de la pantalla. |

## Para ver en pantalla

| # | Cambio | Qué comprobar |
|---|---|---|
| T23 | **Letterbox y fundidos** (`render/stubs.c`: `SetFade`, `SetLetterbox`, `SlideLetterbox`, `UpdateLetterbox`, `DrawLetterbox`, `DrawBlackRectangle`, `HasPending`), con la lógica del escritorio dibujando rectángulos negros por la UI. | Cinemáticas del selector: franjas negras que entran y salen; fundido a negro al empezar y acabar; flyby de TR3; Von Croy de TR2. Que las franjas no tapen la UI de debajo ni queden clavadas al cambiar de nivel. |
| T25 | **Imágenes**: `Output_Overlay_LoadImage` (devuelve si el .565 está en el disco) y `DrawImageInBox` (`DC_PVR_DrawPictureBox`, alfa con la opacidad). | Pantallas de imagen y carga de `phase_picture`; si el disco no trae la imagen la fase termina sola. Comprobar que la caja cae donde el escritorio la pone. |
| T28 | **Líneas con efecto** (`StageLineSegment` en `sprites.c`): tira de dos triángulos planos mirando a la cámara. | Goteo, láseres (TR3 Área 51), lluvia y nieve, electrocución de Lara, dardos. Grosor y color. |
| T30 | **Rayos** (`OutputSource_Lightnings_StageSegment`): dos tiras sumadas, azul y núcleo blanco. | Martillo de Thor (TR1/probable), tormentas de TR3. |

## Flujo normal y títulos (merge con la sesión de TR1)

| # | Cambio | Qué comprobar |
|---|---|---|
| T26b | **Título del TR2 clásico** (`make_disc.sh`): `TITLE.PCX` pasa a `pix/title_eu.565` (en TR3, `TITLEUK`) y `credit*.PCX`/`END.PCX` a sus nombres; el aviso legal lo hace el bloque `TRX_DC_LEGAL`. | Ver el menú con la imagen del título y los créditos. |

## Menú de render y selector de sombras (23/10/2026, compilado, sin ver el menú)

| # | Cambio | Qué comprobar |
|---|---|---|
| T33 | **Fila de sombra de Lara en Gráficos** (`nolua_settings.c`): Octagon, Volume, Soft y Soft (high) (Circle y Sprite se saltan desde `5c6e7f989`); manda sobre el modo, y cambiar de modo repone la del modo. Fila aparte "Enemy shadows" (Original/Volume/Soft). | Que las cuatro salgan en el menú, que cada una dibuje lo suyo, y que Soft en Rendimiento no hunda los fps. |
| T34b | **Fila "Screen" (sin commit)**: 4:3 o 16:9 anamórfico, se aplica en la carga siguiente (`g_DC_WidePending`); gancho `TRX_DC_WIDE`. | Ver 16:9 en un televisor que estira; que el HUD y el menú queden bien. |


## Cambio de modo en marcha y TR1 (23/10/2026)

| # | Cambio | Qué comprobar |
|---|---|---|
| T35b | **Cambiar el modo mientras corre**, probado sin teclas con un disco de prueba (`TRX_DC_SWITCH="<modo> <frame>"` en `make_disc.sh`, `SWITCH.TXT`; modo 3 = los pasos de la fila del menú por Lua). Venecia, Flycast oculto: de Gráficos a Experimental 60 sube de 30 a 58–60 fps y la sombra pasa a Hard; por `trx.config.set` y por `rows.change` igual. La resolución 320×240 no cambia hasta reiniciar el nivel. **Error corregido**: `dc_adaptive_fps` nunca se ponía (el `CONFIG_SET` recibía un `int`, que no llega a un ajuste booleano): faltaba el paso a 30 fps en escenas pesadas. | Con el mando en el menú en C (`nolua_settings.c`): cambiar de modo actualiza las filas y la sombra. Los códigos 0–14 de `TRX_DC_SWITCH` solo funcionan con `DC_LUA=1` (el 60 adaptativo ya no existe). |

## TR1: memoria, texturas VQ y menú (23/10/2026, DC builds 297–310)

| # | Cambio | Qué comprobar |
|---|---|---|
| T37b | **Muestras de TR1 sin bloque en RAM** (`level/sections/audio.c`, DC): cada muestra va del `.phd` a la memoria de sonido al leer los offsets; ya no se reservan 1,4–2,2 MB (el gym) ni los ~0,8 MB de inyecciones que no se usaban. Con `TRX_DC_LEVELS` el gym (nivel 0) y el 3A cargan, y antes abortaban. | Que suenen los efectos de TR1 (no se ha oído). |
| T38b | **Texturas de TR1 en VQ**: `texvq.c` lee los `.phd` (versión 32), pasa las páginas de 8 bits por la paleta como el motor y las añade a `textures.vq` (238 páginas); `make_disc.sh` lo hace también para TR1. Sin VQ la memoria de vídeo bajaba a ~1 MB libres y Flycast se cerraba en el nivel 4. | `texture pages ready (vvv…)` en el log: debe salir `v` en casi todas. |
| T39 | **Menú de TR1**: el fondo `title.webp` sale de `TITLEH.PCX` (y `credits_N` de `CREDN.PCX`); se incluye la música `2.mp3` del título. Antes el disco no llevaba fondo (el gameflow lo llama `title.webp`) ni música (`0 tracks`). | Ver el menú con fondo y música. El TR1 de Steam solo trae las pistas 2 a 9: el resto (la 26, la 57...) sonará mudo hasta tener el juego completo. |

## Logo en la pantalla de la VMU (30/10/2026, compilado, sin CDI)

| # | Cambio | Qué comprobar |
|---|---|---|
| T72 | **Logo "TR" en la pantalla de la VMU al arrancar** (`DC_VMU_DrawLogo` en `vmu.c`, llamado tras `DC_VMU_RestoreSaves` y al entrar en cada menú principal, `GF_RunTitle`, para tapar lo que dejó el último guardado): las letras y el marco del icono del guardado, centrados en los 48×32, con `vmu_set_icon` (todas las VMU puestas). Se dibuja una vez y queda hasta apagar; no se toca en el bucle del juego. | Flycast con la pantalla de la VMU visible: el "TR" sale derecho (no espejado ni al revés) y el arranque no se nota más lento. Consola real: igual. |
| T73 | **Guardado en la pantalla de la VMU y beep** (`M_Store` en `vmu.c`, solo partidas, no `dcopts.txt`): antes de `vmufs_write` la tarjeta muestra "SAVING... / DO NOT / REMOVE" (fuente 4×6 de KOS, `vmufb_present`, que gira la imagen según el mando); después "GAME SAVED" + beep de 150 ms (`vmu_beep_raw` 0x65F0, el valor del ejemplo de KOS) o "NOT SAVED" sin beep. El texto queda hasta el siguiente guardado. Bloquea ~0,2 s más el guardado, que ya bloquea. | Guardar desde el pasaporte y con guardado rápido: textos derechos y legibles, se oye el beep y se corta solo (en consola real: que no quede sonando). Tarjeta llena: "NOT SAVED". |
| T74 | **Secretos contados en Dreamcast** (`level/pipeline.c`, `M_CompleteSetup`, solo DC): el arranque no cuenta los niveles (`Stats_CalculateMaxStats` salta a `finish`), así que los máximos quedaban a cero y ni los secretos de suelo (TR1, TR3) ni los objeto (dragones de TR2) ponían su bit: sin música de secreto, contador a 0 y el premio de "todos los secretos" a la primera. Ahora se cuenta el nivel al cargarlo (`Stats_ScanLevel`, tras objetos y portadores) y los objetos secreto, ya inicializados, reciben su bit (`Pickup_SetSecretMask`). | TR2: coger un dragón, suena la música, el pasaporte cuenta 1/3 y no da premio hasta los tres. TR3: pisar un secreto, suena y cuenta. Cargar una partida con secretos cogidos: siguen contados. Tiempo de carga sin cambio apreciable. |
| T75 | **Pantalla de la VMU en juego** (`vmu_hud.c`, llamado desde `Game_Control` fuera de las demos): L + R + B pasa logo (por defecto) → estadísticas del nivel (TIME, DIST, SHOT aciertos/disparos, SECR encontrados/total; 2 veces por segundo) → buscador de secretos (5 veces por segundo: flecha desde el centro hacia el secreto pendiente más cercano respecto a la cámara, metros abajo, UP/DN si está a más de un sector de altura; beep cada 5/3/2 s a 10/6/3 m (`M_FAR`/`M_MIDDLE`/`M_NEAR`), silencio más lejos; un secreto a ≤10 m pita y muestra el radar 1 s en cualquier pantalla) → logo. Aviso "VMU: …" en la consola del juego. Tras guardar, el texto del guardado se queda 3 s. Nada espera al bus Maple (un comando con la tarjeta ocupada se descarta y se repite). B no hace lo suyo con L + R pulsados. | Flycast con `rend.FloatVMUs`: las tres pantallas, la flecha gira con la cámara y apunta bien (no espejada), los beeps se aceleran al acercarse y se callan lejos y al encontrarlo; "NO SECRETS LEFT" al terminar. Consola real: el beep se corta (100 ms) y no se notan tirones. |

## Arreglos de la VMU y cajas de colisión (30/10/2026, DC builds 397+)

| # | Cambio | Qué comprobar |
|---|---|---|
| T76 | **Cuelgue al guardado rápido** (T73): `M_WaitIdle` (`vmu.c`) bloqueaba y desbloqueaba el frame Maple de la tarjeta, y `maple_frame_unlock` exige un frame respondido (`ASSERT maple_queue.c:204`, visto en el log serie). Ahora solo mira `frame.state == MAPLE_FRAME_VACANT`. | L + R + Start y guardar desde el pasaporte: sin cuelgue, "SAVING..." → "GAME SAVED" + beep. |
| T77 | **Porcentaje de carga en la VMU** (`DC_VMU_ShowLoading`, llamado desde `M_Draw` de `loading.c`): "LOADING" + `47%` + barra de 42 columnas; solo se envía al cambiar el porcentaje. Al acabar vuelve el logo (`DC_VMU_DrawLogo` espera a que la pantalla esté libre). Desde `5c6e7f989` la barra corre hasta cada valor nuevo en vez de saltar. | Cargar un nivel y la partida: sube la barra sin frenar la carga; al terminar sale el logo. |
| T78 | **Cajas de colisión de objetos** (`render/debug_boxes.c`, antes eran stubs vacíos): L + R + Arriba enciende `debug.enable_debug_bounding_boxes` y se dibujan las 12 aristas de cada caja (Lara, objetos, plataformas) en rojo, sin test de profundidad. No son las cajas de salas de `DC_PROBES=1` (esas siguen con Arriba solo, en builds con `DC_PROBES=1` o `DC_BOXES=1`, carpeta `build/dc-*-boxes`). | Encender: cajas rojas sobre Lara y enemigos; se mueven con ellos; apagar. Máximo 256 por cuadro. |

## No hecho (queda en TAREAS.md)

Ver TAREAS.md: bloque C (9–13), D (15 y 16), E17 (salas de color sin mínimo de
sombra ni parpadeo), E20 (reverb y filtro bajo el agua) y G 31 (dibujos de
depuración). El cielo (`Sky_StageLayer`/`StageFogGradient`) no aplica: solo lo
llama TR4.
