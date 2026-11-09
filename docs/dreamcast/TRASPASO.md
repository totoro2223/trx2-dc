# Traspaso: cómo continuar el port de TRX a Dreamcast

Estado a 9 de noviembre de 2026 (commit `5c6e7f989`). Este documento es para la persona (o el
Claude) que retome el trabajo: entorno, ciclo, arquitectura y lecciones.
Los demás documentos tienen un papel cada uno:

- [PROBADO.md](PROBADO.md): qué se probó y su último resultado (OK, FALLIDO,
  SIN VER). **Consúltalo antes de proponer nada.**
- [TAREAS.md](TAREAS.md): solo lo pendiente.
- [REGISTRO.md](REGISTRO.md): historia detallada, con builds y cifras.

## 1. Qué es esto

Port de [TRX](https://github.com/LostArtefacts/TRX) (motor de TR1–TR3 de
LostArtefacts) a Sega Dreamcast con KallistiOS: TR1, TR2 y TR3 con sus
expansiones (Unfinished Business, Golden Mask, Lost Artefact), también en un
disco de tres juegos. Debe correr en Flycast y en consola real con GDEMU
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
| Repo | `D:\trx\trx2-dc` |
| Dependencias de terceros | `D:\trx\deps` (hermano del repo, **no** está en git) |
| DreamSDK R4 | `D:\DreamSDK` (variable `DREAMSDK_HOME`; `dcsh.ps1` cae en `G:\DreamSDK` si falta) |
| Flycast | `D:\Flycast\flycast.exe` (variable `TRX_FLYCAST`; BIOS real desde el 27/10; `Debug.SerialConsoleEnabled` y `rend.FloatVMUs` en `emu.cfg`) |
| Datos clásicos (Steam) | `D:\Program Files (x86)\Steam\steamapps\common\Tomb Raider (I)`, `Tomb Raider (II)`, `TombRaider (III)` |
| Remaster I–III | no está en este PC (`make_disc_all.sh` lo busca en `/h/GOG/…`; `TRX_DC_REMASTER`) |

(Hasta el 23/10/2026 el entorno estaba en `G:` y `H:`, que ya no existen.)

- Git está en `C:\Program Files\Git\cmd\git.exe`. No hay Python en Git Bash
  (los scripts `.py` se usan desde el Python de Windows, si hace falta). Usar
  `python`, no `python3` (cae en el alias de la Microsoft Store).
- El usuario habla en **español**. Quiere las preguntas **de una en una**.

### Preparar `deps/` en una máquina nueva

`build.sh` busca por defecto `../deps`. Contenido necesario:

- `deps/uthash/*.h` (uthash 2.3.0)
- `deps/pcre2-10.49/` (fuentes de PCRE2 10.49)
- `deps/dl/lua-5.5.0.tar.gz` (mismo tar que usa kos-ports; se compila un
  `luac` de PC con `LUA_32BITS`)
- `deps/dl/libmpeg-2.0.0.tar.gz` (decodificador de las películas MPEG-1)
- En DreamSDK: kos-ports `zlib` y `sh4zam` instalados (`lua` solo para
  `DC_LUA=1`).

Después, desde el shell de DreamSDK en la raíz del repo:

```
tools/dreamcast/build_deps.sh ../deps
```

Genera `deps/dc/{include,lib}` (pcre2, uthash, pl_mpeg) y `deps/host/luac.exe`.

## 3. Ciclo de trabajo

Desde PowerShell, en la raíz del repo:

```
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh minsize"
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/make_disc.sh build/dc-minsize '/d/Program Files (x86)/Steam/steamapps/common/Tomb Raider (II)/data' build/dc-minsize/TRX-prof.cdi 1"
.\tools\dreamcast\run_flycast.ps1 -Cdi build\dc-minsize\TRX-prof.cdi -Seconds 60
```

- `build.sh [debug|debugoptimized|minsize|release]`: la build que se usa es
  **`minsize`** (`build/dc-minsize`). Variables de `build.sh`, cada una en su
  carpeta (`build/dc-minsize-<sufijo>`): `DC_PROBES=1` (-probes),
  `DC_PROFILE=1` (-prof), `DC_CENSUS=1` (-census), `DC_LUA=1` (-lua, el juego
  con scripts), `DC_ISO_DMA=1` (-dma), `DC_BOXES=1` (-boxes), `DC_MAGENTA=1`
  (-magenta), `DC_STRICT=1` (-strict, con `DC_TEST_HANG=N`).
- **Discos para GDEMU**: `build.sh release` (`build/dc-release`, sin
  watchdog ni las líneas `[frame]`/`[probe]`/`[bins]` por el puerto serie, que
  en consola paran el juego) y `make_disc.sh build/dc-release …` sin variables
  `TRX_DC_*`. En TR1 hay que pasar `TRX_DC_LEGAL` con el `EIDOSPC.PCX` del
  original: ffmpeg no lee el `LEGAL_1.DDS` del remaster.
  - `dc_sq_vertices` y `dc_xform_pref` (emisión por store queues y `pref` en
    la transformación, vistos en TR4): probados en consola, sin ganancia
    (FALLIDO, builds 1219/1221). Quedan apagados; no se repiten sin otro dato.
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
  solo si el usuario lo pide o si hace falta probar en Flycast (cientos de MB y
  sube el número de build).
- `run_flycast.ps1` guarda el log serie en `build/dc-serial.txt`. `-Hidden`
  (escritorio oculto y sonido a un dispositivo mudo: **las pruebas se hacen
  así**, sin sonido audible), `-StopOn '<regex>'`, `-Flycast` (por defecto
  `TRX_FLYCAST`).
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
  atasca bajo Flycast); `dc_iso_dma` (`DC_ISO_DMA=1`) lee por DMA, que va en
  consola y se cuelga en Flycast.
- `render/`: renderer PowerVR propio en lugar de OpenGL 3.
  - `batcher.c`: transformación (`ftrv`, `fipr`, `fsrra`), iluminación,
    niebla por vértice, recorte, emisión. Lazos rápidos para salas bajo niebla
    negra y para objetos iluminados.
  - `pvr.c`: listas (opaca, punch-through, translúcida, volúmenes), envío por
    store queues al final del frame, caché de cabeceras PVR, texturas
    (twiddled o VQ), mapa de entorno, páginas de relieve.
  - Sombras: `soft` (render-to-texture a un mapa de 1024×256: celdas de 64
    texels, 96 en Soft (high), y hasta 4 enemigos) y `volume` (volúmenes
    modificadores, también para enemigos con "Enemy shadows").
  - Cel shading (punch-through con alpha test), reflejos en las pistolas
    (env map), bump mapping en caras cercanas.
- `dc_mode.c`: modos Gráficos (por defecto) / Rendimiento / Original /
  Experimental 60 (320×240). Al elegir uno, `DC_Mode_ApplyDefaults` pone
  "Frame rate" (`rendering.dc_fps`: 30 / 60 interpolado; el adaptativo se quitó
  en `5c6e7f989`), cel shading, relieve, filtro de texturas y sombra; el
  jugador puede cambiarlos después (el render solo lee las opciones). Menú:
  anillo de opciones > Gráficos (`nolua_settings.c`), filas "Mode" y
  "Restart level".
- Sin Lua por defecto (`dc_lua=false`, [PLAN_SIN_LUA.md](PLAN_SIN_LUA.md)):
  HUD, anillo, pasaporte, ajustes y pantallas están en C (`nolua_*.c`) y
  `nolua.c` responde al motor. Con `DC_LUA=1` vuelve el juego con scripts: Lua
  en su propia región (`dc_lua_alloc.c`) y bytecode `luac -s`
  (`tools/embed_trx_lua.py`).
- `vmu.c`: guardado en VMU (refleja los ficheros de partida del RAM disk) y
  el archivo de opciones `trdctriopt` (con el último juego jugado,
  `dc_settings.c`),
  logo en la pantalla de la VMU y aviso + beep al guardar. `vmu_hud.c`: la
  pantalla de la VMU en juego (L + R + B: logo / estadísticas / buscador de
  secretos). Flycast la muestra con `rend.FloatVMUs = yes` en `emu.cfg`.
- En DC el anillo de opciones tiene pasaporte y Gráficos (una sola página);
  Controles/Juego se reimplementarán más adelante. (Sin commit el 09/11: página
  Sonido con la fila "Soundtrack", Original / TR Suite.)

## 5. Memoria (el límite principal)

16 MB de RAM, ~13 MB de heap alcanzable. En Venecia (`boat.tr2`), jugando:
12 938 K → **10 750 K** tras las reducciones (build 158), ~2,4 MB hasta el
límite. Reducciones hechas: vértices de malla de 36 → 28 B, índices
`uint16_t`, sin `texture_zw` en caras, fotogramas de animación empaquetados
(3 ángulos de 10 bits en `uint32`, `Anim_GetFrameRots`), cadenas sin
duplicar, región Lua, scripts Lua compilados y sin depuración, pantallas Lua
cargadas bajo demanda, texturas en VRAM (VQ libera ~2,6 MB de VRAM, no RAM
principal).

Sin Lua, la región del nivel queda acotada a 7424 K (`M_REGION_MAX`,
`dc_level_heap.c`) y deja ~2,3 MB al montón de newlib (PROBADO, 02/11/2026).
Las cifras de Venecia de arriba son de la build 158, con Lua.

## 6. Rendimiento

Cifras actuales y todo lo descartado, en [PROBADO.md](PROBADO.md). Flycast no
modela cachés ni bloqueos del pipeline: las optimizaciones de `pref`, ramas o
caché solo se notarán en consola real.

## 7. Pendientes

Ver [TAREAS.md](TAREAS.md).

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
- `signatures.lua` en DC (solo builds con `DC_LUA=1`): `--lean-signatures` conserva los miembros
  (`helpers.lua` los necesita); quitarlos rompe el arranque.

## 9. Cómo trabajar con el usuario

- Español, preguntas de una en una.
- Tras cada cambio: compilar. CDI solo si lo pide o hace falta probar.
- Citar "DC build N" con cada CDI nuevo.
- Commits en la rama `dreamcast` con prefijo `dreamcast: ` y descripción
  breve (ver `git log`).

## Ganchos de prueba y disco de tres juegos (24/10/2026)

Variables de `make_disc.sh` para discos de prueba (cada una escribe un `.TXT`
que lee `dc_main.c`; la lista completa está en la cabecera de `make_disc.sh`):
`TRX_DC_FIRE`, `TRX_DC_GIVE`, `TRX_DC_EXIT`, `TRX_DC_COMPLETE`,
`TRX_DC_LEVELS="<último> <fotogramas>"`, `TRX_DC_BENCH`/`DEMO`, `TRX_DC_MODE`,
`TRX_DC_FPS`, `TRX_DC_AI`, `TRX_DC_SNOW`, `TRX_DC_FOG`,
`TRX_DC_ASMOFF`/`ASMOFF2`, `TRX_DC_TITLELIST`, `TRX_DC_LOG`. **Sin Lua (build
por defecto):** `TRX_DC_SAVE` no guarda (llama a `trx.savegame.save` por
`LUA_Eval`, que falla) y `TRX_DC_SWITCH` solo admite 100 (Abajo en el título)
y 101 (expansión); los códigos 0–14 necesitan `DC_LUA=1`.

`tools/dreamcast/make_disc_all.sh <build> [salida.cdi|.iso]` hace el disco de
los tres juegos con sus expansiones (datos del remaster; con películas ~1,8 GB,
`TRX_DC_FMV=0` 1 GB; `TRX_DC_ALL_GAMES` elige cuáles). Cada juego va en `g1`,
`g2` o `g3` (`TRX_DC_SUBDIR`) y la raíz lleva `GAMES.TXT`. `dc_games.c` elige
el juego: en frío, el último jugado según `trdctriopt` de la tarjeta; tras una
caída, la nota en RAM alta (`0xACFF0100`), que también cuenta las caídas
seguidas (`M_Recover`). Cambiar de juego no reinicia la consola (el motor
termina su hilo). Todo lo que se abría en `/cd/...` pasa por `g_DC_Root`.

## Memoria entre niveles: cómo se vigila

`dc_level_heap.c` reparte la región de la partida: el nivel por abajo, lo que
vive más que un nivel por arriba, y **lo pequeño (hasta 16 KiB) que vive más
que un nivel, fuera, en el montón de newlib**. En cada carga el registro dice
`game heap: <tamaño>K, <usado>K in <n> blocks, largest free <x>K`: el usado
debe quedar plano de un nivel a otro; si crece, hay una fuga.

Para ver quién tiene lo que queda: `DC_CENSUS=1 bash tools/dreamcast/build.sh
minsize` (carpeta `build/dc-minsize-census`, define `TRX_DC_HEAP_CENSUS`;
necesita `TRX_DC_WATCHDOG`, que llevan todas las compilaciones salvo
`release`; un `meson configure` a mano lo deshace el siguiente `build.sh`; la región anota quién pidió cada bloque y vuelca la lista y
su disposición en cada carga) y pasar el registro de un disco de prueba con
`TRX_DC_LEVELS="<último> 200"` por
`tools/dreamcast/live_census.py <registro> TRX.elf sh-elf-addr2line.exe`.
`GameBuf_Check` compara los trozos de una arena con los hechos.

Pruebas de guardado (`TRX_DC_SAVE`, solo con `DC_LUA=1`): **mover antes el
archivo de la VMU del emulador** (`D:\Flycast\data\T-TRX00_vmu_save_A1.bin`)
porque es la tarjeta de las partidas del usuario (todos los discos comparten
`T-TRX00`). Las pruebas del 24/10/2026 ya escribieron en sus ranuras 0-4; la
copia de antes está junto a ella (`…user-backup-20261024`).

## Log serie

Los discos normales no imprimen por el puerto serie (bloquea el juego a la velocidad del puerto). Los discos de prueba con algún gancho `TRX_DC_*` lo conservan; para uno sin gancho, `TRX_DC_LOG=1` en `make_disc.sh`. Perfil de carga: `DC_PROFILE=1 bash tools/dreamcast/build.sh minsize` y `prof_report.py`.
