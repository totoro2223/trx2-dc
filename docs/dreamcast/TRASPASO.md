# Traspaso: cómo continuar el port de TRX a Dreamcast

Estado a 21 de octubre de 2026, **DC build 202**. Este documento es para la
persona (o el Claude) que retome el trabajo. Complementa
[AVANCE.md](AVANCE.md), que es el resumen del 18 de octubre y ya se ha
quedado corto en varias áreas.

## 1. Qué es esto

Port de [TRX](https://github.com/LostArtefacts/TRX) (motor de TR1–TR3 de
LostArtefacts) a Sega Dreamcast con KallistiOS. Primero Tomb Raider II,
después Tomb Raider III. Debe correr en Flycast y en consola real con GDEMU
(imagen CDI).

- Rama de trabajo: `dreamcast`, sobre `develop` de upstream.
- Todo el código propio de Dreamcast está en `src/platform/dreamcast/` y
  `tools/dreamcast/`. Los cambios en código compartido van protegidos con
  `#if __DREAMCAST__` (o `TRX_DREAMCAST` en Meson) siempre que cambian el
  comportamiento en PC.
- **Los datos del juego nunca entran en el repositorio.** `make_disc.sh` los
  toma de la copia del jugador al hacer el CDI.
- Tampoco entran: CDIs ni nada de `build/`, herramientas que se pueden
  descargar (DreamSDK/KallistiOS, Flycast, `deps/`), ni recursos con
  copyright del juego (texturas, sonidos, FMV, `textures.vq`, capturas).

## 2. Entorno

| Cosa | Dónde (PC del usuario) |
|---|---|
| Repo | `G:\Tomb Raider dreamcast\TRX` |
| Dependencias de terceros | `G:\Tomb Raider dreamcast\deps` (hermano del repo, **no** está en git) |
| DreamSDK R4 (KOS 2.3.0, GCC 13.2) | `G:\DreamSDK` |
| Flycast | `G:\Flycast\flycast.exe` (con `Debug.SerialConsoleEnabled = yes`) |
| Datos TR2 (GOG) | `H:\GOG\Tomb Raider I-III Remastered\2\DATA` |
| Datos TR3 (Steam) | `G:\SteamLibrary\steamapps\common\TombRaider (III)\data` |

- Git está en `C:\Program Files\Git\cmd\git.exe`. No hay Python en Git Bash
  (los scripts `.py` se usan desde el Python de Windows, si hace falta).
- El usuario habla en **español**. Quiere las preguntas **de una en una**.

### Preparar `deps/` en una máquina nueva

`build.sh` busca por defecto `../deps`. Contenido necesario:

- `deps/uthash/*.h` (uthash 2.3.0)
- `deps/pcre2-10.49/` (fuentes de PCRE2 10.49)
- `deps/dl/lua-5.5.0.tar.gz` (mismo tar que usa kos-ports; se compila un
  `luac` de PC con `LUA_32BITS`)
- En DreamSDK: kos-ports `lua` y `zlib` instalados.

Después, desde el shell de DreamSDK en la raíz del repo:

```
tools/dreamcast/build_deps.sh ../deps
```

Genera `deps/dc/{include,lib}` (pcre2, uthash) y `deps/host/luac.exe`.

## 3. Ciclo de trabajo

Desde PowerShell, en la raíz del repo:

```
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh minsize"
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/make_disc.sh build/dc-minsize '/h/GOG/Tomb Raider I-III Remastered/2/DATA' build/dc-minsize/TRX-prof.cdi 1"
.\tools\dreamcast\run_flycast.ps1 -Cdi build\dc-minsize\TRX-prof.cdi -Seconds 60
```

- `build.sh [debug|debugoptimized|minsize|release]`: la build que se usa es
  **`minsize`** (`build/dc-minsize`).
- `make_disc.sh <build> <DATA TR2> [salida.cdi] [nivel]`:
  - Sin 4º argumento: disco del usuario (`TRX.cdi`) con selector de nivel al
    arrancar.
  - `1`: empieza directamente en La Muralla China. **Usar siempre en los
    discos de prueba propios**, y con otro nombre (`TRX-prof.cdi`) para no
    pisar el del usuario.
  - `"1 8"`: carga las partidas 0 y 1 alternando, 8 veces (prueba de
    fragmentación; ver `DC_Watchdog_TakeTestLoad`).
  - Incrementa `build/dc-build-number` y lo muestra en la pantalla de carga.
    **Decir siempre al usuario "DC build N"** cuando se genera un CDI.
  - Genera también `textures.vq` con `tools/dreamcast/texvq.c` (compresión
    VQ de las páginas de textura; solo se rehace si cambian las entradas).
- **No hacer un CDI tras cada cambio.** Tras un cambio, solo compilar. CDI
  solo si el usuario lo pide o si hace falta probar en Flycast (~115 MB y
  sube el número de build).
- `run_flycast.ps1` guarda el log serie en `build/dc-serial.txt`.
  `-KeepRunning` si el usuario va a jugar a mano. `play_latest.ps1` arranca
  el CDI más reciente.
- `flycast_keys.ps1` (`-Action Down/Up` para mantener teclas) y
  `flycast_shot.ps1` para pruebas automáticas. Teclado de Flycast:
  flechas = cruceta, X = A, S = X (saltar), C = B, D = Y, Enter = Start,
  Espacio = avance rápido.
- Inventario en pruebas: Enter, ~2 s, Abajo, ~1,5 s, Izquierda (Gráficos está
  a un paso a la izquierda del pasaporte), ~1,5 s, X. Esperas cortas.

### Reglas con Flycast

- El PC tiene dos pantallas: proyector JMGO (principal, por defecto en los
  scripts) y TV LG. Preguntar si no está claro.
- **No enviar teclas, capturar ni cerrar un Flycast que el usuario haya
  abierto para jugar.** Comprobar antes `Get-Process flycast`. Los scripts de
  teclas se niegan a actuar si Flycast no tiene el foco.
- La línea `Flycast has stopped: Fatal: SH4 exception when blocked` que sale
  en todas las ejecuciones es antigua; no indica un cuelgue. Comprobar si
  `flycast.exe` sigue vivo.
- Un `SH4 exception when blocked` real suele ser `arch_abort` tras un OOM o
  un assert: mirar el log serie.

### Diagnóstico

- `[watchdog]` cada 10 s: heap, `brk`, pila, región Lua (small/big/spilled).
- `[frame]` cada 120 frames: tiempos en 0,1 ms (logic, stage, pass, xform,
  submit, map, wait), polígonos, listas en KiB. **Fiarse de estos tiempos**
  más que del perfilador.
- Perfilador por muestreo: opción Meson `dc_profile=true`; se lee con
  `tools/dreamcast/prof_report.py`. Muestrea a 1 ms y aliasa con los 30 fps.
- `[alloc]`, `[kept]`, `[heap]`: rastreo de reservas (`dc_alloc_trace.c`);
  resolver direcciones con `sh-elf-addr2line`.
- Flycast no modela cachés ni bloqueos del pipeline: las optimizaciones de
  `pref`/ramas solo se notarán en consola real.

## 4. Arquitectura del port (resumen)

- `dc_main.c`: arranque; el motor corre en su propio hilo (la pila principal
  de KOS es de 64 KiB). Selector de nivel en framebuffer
  (`dc_level_select.c`).
- `kos/fs_iso9660.c`: copia de KOS con lectura por PIO (el DMA del GD-ROM se
  atasca bajo Flycast).
- `render/`: renderer PowerVR propio en lugar de OpenGL 3.
  - `batcher.c`: transformación (`ftrv`, `fipr`, `fsrra`), iluminación,
    niebla por vértice, recorte, emisión. Lazos rápidos para salas bajo niebla
    negra y para objetos iluminados.
  - `pvr.c`: listas (opaca, punch-through, translúcida, volúmenes), envío por
    store queues al final del frame, caché de cabeceras PVR, texturas
    (twiddled o VQ), mapa de entorno, páginas de relieve.
  - Sombras: `soft` (render-to-texture 512×64, una por frame) y `volume`
    (volúmenes modificadores).
  - Cel shading (punch-through con alpha test), reflejos en las pistolas
    (env map), bump mapping en caras cercanas.
- `dc_mode.c`: modos Gráficos (por defecto) / Rendimiento / Experimental 60
  (320×240). Menú: Opciones > Gráficos > pestaña Rendering, fila "Mode" y
  luego "Restart level" (`data/trx/ship/modules/ui/dc_restart.lua`).
- `dc_lua_alloc.c`: Lua en su propia región (2,5 MB) para no fragmentar el
  heap. Scripts embebidos como bytecode `luac -s` (`tools/embed_trx_lua.py`,
  opción `lua_compiler`).
- `vmu.c`: guardado en VMU (refleja los ficheros de partida del RAM disk).
- En DC el anillo de opciones solo tiene pasaporte + Gráficos, y Gráficos
  solo la pestaña Rendering (recorte de memoria; Sonido/Controles/Juego se
  reimplementarán más adelante).

## 5. Memoria (el límite principal)

16 MB de RAM, ~13 MB de heap alcanzable. En Venecia (`boat.tr2`), jugando:
12 938 K → **10 750 K** tras las reducciones (build 158), ~2,4 MB hasta el
límite. Reducciones hechas: vértices de malla de 36 → 28 B, índices
`uint16_t`, sin `texture_zw` en caras, fotogramas de animación empaquetados
(3 ángulos de 10 bits en `uint32`, `Anim_GetFrameRots`), cadenas sin
duplicar, región Lua, scripts Lua compilados y sin depuración, pantallas Lua
cargadas bajo demanda, texturas en VRAM (VQ libera ~2,6 MB de VRAM, no RAM
principal).

El menú Gráficos sube Lua de ~1,16 MB a ~1,59 MB por el framework de
diálogos Lua (ver pendientes).

## 6. Rendimiento

30 fps estables en La Muralla China y Venecia (Flycast). Venecia, build 200:
xform 6,0 ms, pass 17,0 ms, ~5,7 ms de margen. 60 fps reales necesitarían
~16,6 ms totales (ahora ~27,6): no es alcanzable sin recortar.

Probado y **descartado** (sin ganancia en Flycast): `-O2` por fichero o en
una librería estática para `batcher.c`/`pvr.c`; `DC_PVR_Depth3D` inline;
lógica a 15 Hz (los ticks de TR son de 1/30 s fijos, el usuario lo rechazó);
fusionar el pase de cel shading con la emisión opaca (builds 201–202).

## 7. Pendientes, por prioridad aproximada

1. **Cuelgue en cargas repetidas** (fragmentación). Arreglos en builds
   77–93; 5+ cargas OK. Queda: un cuelgue dentro del recorrido del heap en la
   carga 2 (build 91) — confirmar si es corrupción del heap (un assert
   `vertex_count > 0` de la build 82 nunca se explicó). Luego 8+ cargas con
   el disco `"1 8"`, cargas desde el pasaporte a mano y cambios de nivel.
2. **Experimental 60**: franja negra arriba a 320×240; comprobar la vuelta a
   640×480; medir Rendimiento; cambios de nivel; quitar el define temporal
   `TRX_DC_MMU_NULL_TRAP` marcado `# DBG` en
   `src/platform/dreamcast/meson.build`.
3. **Rendimiento del inventario**: 9 → 22 fps; tras la arena de bloques
   pequeños de Lua (build 74) no se ha vuelto a medir. Medir con `dc_profile`.
4. **Probar en consola real (GDEMU)** y medir; Flycast no modela cachés.
5. **Audio** (AICA, 2 MB propios; en RAM principal hace falta un buffer por
   muestra y buffers de streaming de 64–256 K). El ERR
   `Unexpected sample header` ya existía.
6. Candidatos de memoria: datos crudos de las salas tras construir las
   mallas (~1,7 MB, pero `shadows.c` compartido aún lee caras), tablas de
   cadenas.
7. Ideas de rendimiento: culling por trozos de sala (~1–1,5 ms como mucho),
   lógica de juego/Lua (~4 ms).
8. Aparcado por el usuario: pantalla Rendering mínima propia de DC en lugar
   del diálogo Lua (~+50 K en vez de +430 K); mipmaps para el mundo;
   subir `vertex_buf_size` de PVR (512 K) ahora que sobra VRAM.
9. Resto de deseos del usuario: gatillo L como modificador (distribución por
   acordar; bengala con L+Y), textura del cielo, efectos, FMV, trenza y armas
   largas a la espalda (versión reducida de `lara_outfits`), Sonido/Controles
   /Juego en el menú, resto de niveles de TR2 y luego TR3.

### Cosas hechas que el usuario aún no ha visto en pantalla

Conviene confirmarlas con él: reflejos de pistolas con la intensidad bajada
(build 113), sombra del cel shading según la luz (116), relieve más intenso
y desactivación automática por fps (124–140), culling por portal/borde de
pantalla (73–76: buscar caras que falten en los bordes).

## 8. Lecciones que costaron caro

- Render-to-texture en Flycast: el destino debe ser al menos tan ancho como
  alto (64×128 sale negro); textura con stride más estrecha que el stride
  declarado cierra Flycast; cada escena RTT cuesta ~un vblank, así que una
  sola por frame.
- Bump mapping: el resultado del PVR va en el **alfa**; usar
  `PVR_TXRENV_REPLACE`, no `DECAL` (con DECAL no se ve nada).
- Flycast solo usa mipmaps con filtrado activado.
- El mutex de `malloc` de newlib es caro en KOS (`mutex_unlock` siempre llama
  a `genwait_wake_one`): de ahí la arena sin bloqueos para Lua.
- `signatures.lua` en DC: `--lean-signatures` conserva los miembros
  (`helpers.lua` los necesita); quitarlos rompe el arranque.

## 9. Cómo trabajar con el usuario

- Español, preguntas de una en una.
- Tras cada cambio: compilar. CDI solo si lo pide o hace falta probar.
- Citar "DC build N" con cada CDI nuevo.
- Commits en la rama `dreamcast` con prefijo `dreamcast: ` y descripción
  breve (ver `git log`).
