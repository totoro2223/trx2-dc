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

## No hecho (queda en TAREAS.md)

Bloque C: 7, 8, 9, 10, 11, 12, 13 (piden medir o investigar). Bloque D: 15 y 16
(exigen separar datos de sala o tocar formatos, riesgoso a ciegas). E17: giro de
chispas, salas de color sin mínimo de sombra ni parpadeo, nombre del guardado
VMU. E20: reverb y filtro bajo el agua (programa del DSP del AICA). G: 26 (quad
de vídeo y `av_stub.c`), 27 (cielo), 31 (dibujos de depuración).
