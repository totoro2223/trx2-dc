# Plan: vídeos (FMV) en MPEG-1 como los de TR4

Preparado el 30/10/2026. Antes de empezar lee [TRASPASO.md](TRASPASO.md) y
**`PROBADO.md`**. **Hecho el mismo día (P1 a P5, DC builds 861-862) sin P0 y
sin medir**. **Estado (09/11/2026):** en uso (`TRX_DC_FMV_CODEC` vale `mpeg1` por defecto en `make_disc.sh`). Reproductor reescrito con libmpeg 2.0.0, solo MPEG-1 (DC builds 948-956): Flycast OK a 15 fps; consola sincronizada, con tirones los primeros 30 s (PROBADO; tarea 73 de `TAREAS.md`). P0 nunca se hizo; P5 a medias (`ancient` 93,4 → 37,9 MB; faltan los tamaños de los tres juegos y la CPU en GDEMU). Detalle en
`REGISTRO.md`.

## Qué usa TR4 en Dreamcast (visto en su disco)

- 13 películas `FMV/DC_FMV00..12.SFD` (**449 MB** en total, de 5 a 66 MB).
- `.SFD` es Sofdec (CRI): un MPEG-2 PS con **vídeo MPEG-1 320x240, 25 fps,
  2.000 kb/s** y **audio ADX estéreo 44,1 kHz, 396 kb/s** (comprobado con
  ffprobe en `DC_FMV12`).
- Sofdec es propietario y no hace falta: lo que importa es el flujo de bits,
  MPEG-1, que es abierto. El audio ADX no aporta nada (ver más abajo).

## Qué hacemos hoy (`tools/dreamcast/fmvconv.c`, `video.c`)

- Cada fotograma es una textura **VQ de 256x256** (18 KiB fijos, sin
  compresión entre fotogramas) más su trozo de audio Yamaha ADPCM, que el AICA
  decodifica solo.
- Tamaño en disco: TR1 223 MB, TR2 221 MB, TR3 remasterizado 324 MB, TR3
  clásico 318 MB (30 fps). Unos 0,26 MB/s de imagen a 15 fps.
- Calidad: la imagen se aprieta a un cuadrado de 256x256 y el VQ de bloques de
  2x2 deja artefactos visibles.

## Idea y ahorro esperado

MPEG-1 comprime también entre fotogramas. A 15 fps y 320x208 bastan ~600-900
kb/s de imagen (TR4 usa 2.000 kb/s a 25 fps), frente a los ~2.100 kb/s de hoy:

| | Hoy | MPEG-1 (estimado) |
|---|---|---|
| Imagen | 0,26 MB/s | ~0,08-0,11 MB/s |
| TR1 + TR2 + TR3 rem. | ~770 MB | ~400 MB |

Son estimaciones: **P0 las mide**. Ventajas: discos más pequeños (importa para
el disco de los tres juegos), sin apretar a 256x256, menos artefactos que el
VQ. Coste: la CPU decodifica el vídeo (hoy no decodifica nada) y hay que
mantener 15 fps (TR3 clásico, 30).

## Piezas que ya existen

- `kos-ports/libmpeg` (MIT, fork de pl_mpeg para Dreamcast: decodifica vídeo
  MPEG-1 y audio MP2) y `kos-ports/libADX`. **No están compilados** aquí
  (`kos-ports/lib` solo tiene GL, Lua y zlib). Hay que comprobar si el fork
  envía los macrobloques al convertidor YUV del PVR (`pvr_dma.h`, TA YUV) o
  convierte en la CPU a RGB565.
- El reloj de audio y la cola de sonido de `video.c` (AICA, ADPCM) se quedan.
- `make_disc.sh` ya recorta, cachea por película y elige fps y tamaño.

## Decisiones

- **Vídeo**: MPEG-1, sí.
- **Audio**: seguir con Yamaha ADPCM. Tiene el mismo caudal que ADX (352 kb/s)
  y el AICA lo decodifica sin CPU; ADX y MP2 gastarían CPU. MP2 a 128 kb/s
  ahorraría ~30 KB/s de audio, pero es una fase aparte y opcional.
- **Contenedor**: el nuestro (`TRXV` versión 3): cabecera, tabla de trozos y,
  por trozo, el flujo MPEG-1 y su audio ADPCM intercalados, para leer disco en
  una sola pasada como hoy. El `.mpg` crudo de ffmpeg se trocea al muxear.

## Tareas

### P0. Experimento sin código (primero, decide el resto)

Con ffmpeg (`-c:v mpeg1video -b:v 700k/1000k -g 15 -bf 0` por probar) codificar
3 películas (una de TR1, una de TR2, una de TR3) y comparar tamaño y calidad
con las actuales (`build/dc-minsize/fmv-*`). Tabla en `REGISTRO.md`. Criterio:
sigue adelante si el ahorro ≥ 40 % y la imagen no se ve peor que el VQ.

### P1. Decodificador en el port

Añadir `libmpeg` a `tools/dreamcast/build_deps.sh` (`deps/dc`). Prueba mínima:
decodificar una película en una build de pruebas y medir ms por fotograma.
**Flycast no sirve para medir la CPU del decodificado** (su dynarec va más
rápido que la SH4 real): hace falta GDEMU. Si la SH4 no llega a 15 fps con
margen, parar aquí.

### P2. Reproductor (`video.c`)

Lector de `TRXV` v3 al lado del v2 (se conservan los discos viejos). Los
buffers del decodificador (3 imágenes YUV de 320x240, ~345 KB) se reservan al
abrir la película y se liberan al cerrarla, sin dejar fragmentación en el heap
del nivel. La salida va a la misma superficie y `upload` de hoy.

### P3. Conversor (`fmvconv.c` o un script con ffmpeg)

Codificar con ffmpeg (tamaño de origen, no 256x256), trocear el flujo en
límites de fotograma y mezclarlo con el audio. Sin B-fotogramas o con orden de
decodificación resuelto en el muxer.

### P4. `make_disc.sh`

Variable `TRX_DC_FMV_CODEC=vq|mpeg1` (por defecto, el que quede mejor tras P1).
Cuidado con la caché `fmv-<juego>-<origen>`: incluir el códec en el nombre.

### P5. Medir y registrar

Tamaño de los tres juegos, fps reales de reproducción en consola, sincronía
audio-vídeo y tiempo de apertura. Fila en `PROBADO.md`, detalle en
`REGISTRO.md`.

## Riesgos

- CPU de la SH4: solo se ve en consola real; Flycast engaña.
- MPEG-1 a bitrate bajo hace bloques en degradados oscuros (los FMV de TR son
  oscuros). Se ve en P0.
- Memoria del decodificador durante la película: poca, pero comprobar con
  `alloc_census.py` que no empeora la fragmentación al volver al nivel.
