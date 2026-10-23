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

## Flujo normal y vídeo (DC build 283)

| # | Cambio | Qué comprobar |
|---|---|---|
| T26a | **Selector**: la primera entrada de cada juego es "Normal flow (logos, title, menu)": arranca como en producción, sin `-l`. `Output_Quad_*` implementado sobre la imagen de pantalla (`DC_PVR_UploadPicture`: cuadro BGRA a 512×512 RGB565, con bandas si el origen no es 4:3). **Sin decodificador**: nada lo llama aún. | Que compile y que una imagen cargada después de un cuadro no reutilice el cuadro (`m_PictureName`). Probar de verdad cuando haya vídeo. |
| T26b | **Imágenes de TR2 clásico** (`make_disc.sh`): `TITLE.PCX`, `legal.PCX`, `credit*.PCX` y `END.PCX` pasan a `pix/*.565` (`title_eu`, `legal_eu`); en TR3, `TITLEUK`→`title_eu`, `LEGAL`→`legal_eu`. Comprobado el contenido de `legal_eu` y, en el log de Flycast oculto, la secuencia legal, título y demo tras 30 s. | Ver en pantalla: aviso legal, el menú con la imagen del título, los créditos. Los logos y la intro son FMV: ver T26c. |

| T26c | **FMV** (compilado, sin disco ni Flycast): `fmvenc.c` (host) cuantifica cada cuadro a 256 bloques de 2×2 (RGB565) con un byte por bloque, 256 de ancho a 15 fps; `make_disc.sh` convierte `fmv/*.RPL` (también avi, mp4, ogv, mkv, webm) con ffmpeg y deja `fmv/<nombre>.fmv` y `<nombre>.wav` (ADPCM Yamaha 22 kHz); `video.c` implementa `Video_*` y `audio.c` abre el `.wav` hermano de la película. `TRX_DC_FMV=0` los omite. | Primer disco: tarda minutos (unos 6000 cuadros en TR2) y crece (~100 MB en TR2); ver que codifica sin errores. En el juego: logo y intro del flujo normal, calidad del VQ por cuadro (parpadeo del libro de códigos), ritmo (reloj de la consola, no el del sonido) y desfase con la voz, saltar con Start, pausa. Posibles fallos: nombre del fichero (`LOGO.AVI` → `LOGO.fmv`, mayúsculas), lectura del disco a ~200 KB/s junto al sonido, coste de convertir 256×192 a la textura de 512×512 por cuadro. |

## No hecho (queda en TAREAS.md)

Bloque C: 7, 8, 9, 10, 11, 12, 13 (piden medir o investigar). Bloque D: 15 y 16
(exigen separar datos de sala o tocar formatos, riesgoso a ciegas). E17: giro de
chispas, salas de color sin mínimo de sombra ni parpadeo, nombre del guardado
VMU. E20: reverb y filtro bajo el agua (programa del DSP del AICA). G: 26 (FMV, ver T26c) y 31 (dibujos de depuración). La 27 (cielo) no aplica: solo TR4.
