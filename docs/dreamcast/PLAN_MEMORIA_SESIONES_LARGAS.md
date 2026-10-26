# Plan: memoria en sesiones largas (niveles seguidos, cargas, guardados, cambio de juego)

Preparado el 26/10/2026 para otro agente. Brief autocontenido: objetivo, estado
de partida, herramientas, batería de pruebas (P1–P13) con criterio de éxito, y
tareas de corrección (M1–M13) según lo que falle. Antes de empezar lee
[TRASPASO.md](TRASPASO.md) (entorno, ciclo de compilación, reglas con Flycast),
y el estado de memoria en [REGISTRO.md](REGISTRO.md) (24/10/2026, "Recorrido de
niveles, guardados y región") y [PRUEBAS_PENDIENTES.md](PRUEBAS_PENDIENTES.md)
(T58–T70).

## Objetivo

Que una consola pueda jugar **sin reiniciar**: todos los niveles de TR1, TR2 y
TR3 de seguido, cargar y guardar partida muchas veces, salir al título y volver,
reiniciar nivel, abrir el menú Graphics y cambiar de juego, sin caída, sin
aserto de `Memory_Alloc`, sin "starting the disc over" y sin que el hueco libre
mayor baje de forma sostenida.

**Criterio de éxito global** (en cada prueba salvo que diga otra cosa):

- Ninguna línea `exception`, `Assertion`, `aborting`, `starting the disc over`.
- `game heap: ... largest free` estable: no baja más de ~50 K por carga a lo
  largo de 10+ cargas (hoy baja ~90 K por carga, build 320).
- Región del nivel: lo retenido entre niveles ≤ 100 K y sin crecer.
- Montón de newlib: `brk` a ≥ 512 K del final de la RAM en el peor nivel (hoy
  124 K en Barkhang).
- Desvíos (reservas de la región que caen a newlib) = 0 en la carga, o pocos y
  sin crecer.

## Estado de partida (lo que ya se sabe)

| Hecho | Dónde |
|---|---|
| Fuga de ~0,9 MB por nivel (`Walkable_Reset`) arreglada | T59 |
| Bloques pequeños que sobreviven al nivel van a newlib (`M_SMALL_MAX` 16 K); pasar de nivel en nivel sin guardar: TR1 14, TR2 17, TR3 12 niveles sin caída | T60 |
| Piscina de bloques pequeños de la carga en la cima de la región (`M_POOL_SIZE`, hoy **384 K**, `dc_level_heap.c`) | T67 |
| Matrices del batcher fuera de la región | T68 |
| Gancho de guardado/carga con ranuras 1–5 (antes nunca cargaba) | T63 |
| Cambio de juego en el mismo proceso; cada juego retiene 287 K (TR2) / 299 K (TR3) sin crecer | T70 |

**Abierto:**

1. Discos `TRX-tr{1,2,3}-run2.cdi` (builds 323–325, piscina 384 K) **nunca se
   corrieron** (y ya no están en `build/dc-minsize`: hay que rehacerlos).
2. TR2 **cae al cargar Barkhang (12)** con guardados: la región se llena (pico
   6591 K) y el guardado siguiente aborta (TAREAS 64).
3. TR3 rozan el tope desde Temple (pico ~6,0 MB); con guardados se seca el
   montón; Flycast se cierra solo en Lud's Gate (11) (TAREAS 65).
4. El hueco mayor baja ~90 K por carga (TR1, build 320).
5. ~2,2 MB de bloques pequeños (< 24 K) de larga vida en newlib, sin dueño
   conocido.
6. Sin efectos de sonido tras cambiar de juego (TAREAS 68).
7. Cada carga redibuja todas las mallas para hallar la transparencia (caché
   `object_transparency_*` nunca se escribe; TAREAS 66), y eso infla el pico.
8. Salir al selector tras el juego sigue reiniciando el disco (`arch_reboot`).
9. Reiniciar nivel / cambiar de modo (320×240) se quedaba sin memoria en
   builds antiguos (`build/dc-serial-restart-crash.txt`); no se ha vuelto a
   probar desde los arreglos de región.
10. Bug aparcado: heap corrupto en la demo del Ganges (TR3), TAREAS 22.

## Herramientas

**Variables de `tools/dreamcast/make_disc.sh`** (se escriben en el disco como
`*.TXT` y las lee `src/platform/dreamcast/dc_main.c`):

| Variable / argumento | Efecto |
|---|---|
| 4º argumento `N` | Empieza en el nivel N (sin selector). |
| 4º argumento `"N K"` | Empieza en N y carga K partidas seguidas (`DC_Watchdog_SetTestLoads`; imprime `[test] load i of K: used= free= lua= small= big=`). |
| `TRX_DC_GAME=tr1\|tr2\|tr3` | Juego del disco. |
| `TRX_DC_LEVELS='<último> <frames>'` | Pasa de nivel en nivel hasta el último, `<frames>` en cada uno (`[levels] on to level N`, `[levels] done`, en `game/phase/executor.c`). |
| `TRX_DC_SAVE=<frames>` | Guarda cada `<frames>` en las ranuras 1–5 y carga la 1 tras el quinto. |
| `TRX_DC_EXIT=<frames>` | Sale del juego a los `<frames>` del nivel (prueba de salida al título). |
| `TRX_DC_SWITCH='<modo> <frame>'` | Cambia el modo de render como el menú (0 Graphics, 1 Performance, 2 Experimental 60). |
| `TRX_DC_GIVE`, `TRX_DC_DEMO`, `TRX_DC_BENCH` | Arma a Lara, demo o benchmark (útiles para correr sin teclas). |

Disco de tres juegos: `tools/dreamcast/make_disc_all.sh <build-dir> [out.cdi]`
(sin vídeos para estas pruebas).

**Líneas del registro serie a leer:**

- `game heap: <usado>K, <K> in <n> blocks, largest free <K>K, ...` y
  `game heap after the injections: ...` (`game/level/pipeline.c`): región del
  nivel en cada carga.
- `[watchdog]` (región de Lua: small/big/spilled, `brk`).
- `[test] load ...` (cargas de partida en serie).
- Con censo: `[kept]` (bloques vivos desde la última carga, con llamadores) y
  `[heap]` (mapa y hueco mayor de newlib), en `dc_alloc_trace.c`.

**Censo:** opción de Meson `-Ddc_heap_census=true` (vuelca la región al
terminar la carga) y `tools/dreamcast/live_census.py <log> <elf> <addr2line>
[índice]` para agrupar por dueño. Para newlib, bajar `DC_ALLOC_TRACE_MIN`
(`dc_alloc_trace.c`, hoy 24 K) a 1–4 K **solo en un build de censo** (el censo
cambia el reparto: no comparar sus cifras de pico con un build normal).

**Correr** (siempre la copia `G:\FlycastTest`, oculto, VMU de prueba):

```powershell
Copy-Item build\vmu_test_card.bin G:\FlycastTest\data\T-TRX00_vmu_save_A1.bin -Force
$env:TRX_FLYCAST = 'G:\FlycastTest\flycast.exe'
Remove-Item build\dc-mem-P2-tr2.txt -ErrorAction SilentlyContinue
tools\dreamcast\run_flycast.ps1 -Cdi build\dc-minsize\T-mem-P2-tr2.cdi -Hidden -Seconds 900 -Out build\dc-mem-P2-tr2.txt -StopOn 'exception [0-9a-f]{4}|Assertion|aborting|starting the disc over|\[levels\] done'
```

Nombres: disco `build/dc-minsize/T-mem-P<n>-<juego>.cdi`, log
`build/dc-mem-P<n>-<juego>.txt`; al terminar, mover los logs a
`build/logs/<fecha>/` con su línea en `INDICE.txt`.

## Fase 0: preparación

- [ ] **0.1** Compilar (`build.sh minsize`) el árbol actual y anotar el "DC
  build N". Hay cambios sin commit en `render/batcher.c` y
  `tools/dreamcast/run_flycast.ps1`: mirarlos antes y no perderlos.
- [ ] **0.2** Escribir un lector de logs `tools/dreamcast/mem_summary.py` (o
  .ps1) que saque, **por carga**: nivel, `game heap` usado / hueco mayor /
  bloques, `after the injections`, desvíos, lo retenido, Lua small/big/spilled,
  `brk` y su distancia al final de la RAM, VRAM libre; y marque la primera línea
  de fallo. Salida en tabla Markdown para pegar en REGISTRO.md. Es la pieza que
  hace comparables todas las pruebas.
- [ ] **0.3** Línea base: correr P1 y P2 con el build actual sin tocar código.

## Batería de pruebas

Cada prueba: disco, cómo correrla, qué medir, cuándo pasa. Las que llevan
"(usuario)" las hace el usuario a mano; el agente prepara el disco y para.

| # | Prueba | Disco | Pasa si |
|---|---|---|---|
| **P1** | **Niveles seguidos, sin guardar** (regresión de T60) | `TRX_DC_GAME=trN TRX_DC_LEVELS='<último> 300'`, 4º arg `0` (o el primer nivel). TR1 último 15, TR2 17, TR3 19. | Llega a `[levels] done` en los tres juegos; hueco mayor estable; retenido ≤ 100 K. |
| **P2** | **Niveles seguidos con guardados y cargas** (lo que eran los `run2`) | P1 + `TRX_DC_SAVE=90`. | Igual que P1 y además: TR2 pasa Barkhang (12), TR3 pasa Lud's Gate (11) y llega al 19; ningún "could not be copied to the memory card: no memory" salvo tarjeta llena. |
| **P3** | **Cargas repetidas en el mismo nivel** | 4º arg `"1 30"` (TR2 Great Wall), y luego `"<nivel pesado> 30"` (TR2 Venecia 2 u Ópera, TR3 Jungle 1). Necesita partidas en la VMU de prueba en las ranuras 0/1: guardarlas antes con P2 o a mano. | 30 cargas sin caída; hueco mayor de la región y `[heap] largest hole` sin tendencia a bajar (pendiente ≤ 5 K por carga). |
| **P4** | **Pico de Barkhang** (TAREAS 64) | `TRX_DC_GAME=tr2 TRX_DC_LEVELS='12 300' TRX_DC_SAVE=90`, 4º arg `11`. Repetir con `-Ddc_heap_census=true`. | Barkhang carga y guarda 3+ veces; pico de la región < 95 % (6080 K de 6399 K). |
| **P5** | **TR3 entero** (incluye TAREAS 65) | `TRX_DC_GAME=tr3 TRX_DC_LEVELS='19 300'`, primero sin y luego con `TRX_DC_SAVE=90`. | Llega al 19; si Flycast se cierra sin nada en el registro, anotar nivel, VRAM libre y últimas 50 líneas. |
| **P6** | **Cambio de juego repetido** (T70, TAREAS 68) | Disco de tres juegos (`make_disc_all.sh`). Hoy el cambio va con Arriba/Abajo en el título: o bien con `flycast_keys.ps1` (necesita la ventana visible, no oculta), o añadir un gancho `TRX_DC_GAMES='<secuencia> <frames>'` que encadene juegos y entre en un nivel de cada uno (ver M10). | 20 cambios TR1→TR2→TR3→…: retenido por juego estable (≤ 300 K, sin crecer); **los efectos de sonido suenan** tras el cambio (buscar en el log los canales de `snd_sfx_chn_alloc` o un contador de efectos lanzados). |
| **P7** | **Salir al título y volver a entrar** | `TRX_DC_EXIT=600` + un gancho que vuelva a entrar al nivel desde el título (si no existe, añadirlo junto a M10). | 20 vueltas título↔nivel sin crecer el retenido ni bajar el hueco. |
| **P8** | **Reiniciar nivel y cambiar de modo** | `TRX_DC_SWITCH='2 900'` (a Experimental 60, recarga el nivel a 320×240) y `'0 900'` para volver; en TR2 Venecia y TR3 Jungle. | El cambio recarga sin OOM (era el fallo de `dc-serial-restart-crash.txt`); el uso tras la recarga ≈ el de una carga limpia (± 100 K). |
| **P9** | **Menú Graphics tras muchos niveles** | Tras P1 hasta el último nivel, abrir el pasaporte/Graphics (teclas con `flycast_keys.ps1`, Flycast visible y con foco: ver [TRASPASO.md](TRASPASO.md)). | Se abre y cierra sin OOM; `spilled` de Lua vuelve a 0 al cerrar; `brk` ≥ 256 K del final. |
| **P10** | **Guardar con poca memoria y VMU llena** (T64) | P4 con la VMU de prueba casi llena (copiar una tarjeta con 12 partidas). | Sale el aviso en el pasaporte y en el registro; no aborta. |
| **P11** | **Sesión larga sin cargar** (fugas por frame) | `TRX_DC_BENCH=0` (demo en bucle) en TR2 Venecia y TR3 Londres, 30 min, `-Seconds 1900`. | `[watchdog]` (usado de newlib, Lua) sin tendencia a subir en 30 min; si sube, censo de bloques pequeños (TAREAS 9: ~30 reservas por frame de `Catalog_CreateKey`, `Memory_DupStr`, `String_Format`). |
| **P13** | **Menú principal → demos → cargar o empezar partida** (el camino de quien deja la consola encendida en el título) | Desde un nivel (tras P1/P2 o tras varias cargas): salir al menú principal, dejar que pasen **una o dos demos** (o lanzarlas con `TRX_DC_DEMO='-1 -1'`: `-1` es la que elegiría el modo demo), volver al título y entonces **(a) cargar una partida** del pasaporte o **(b) empezar una partida nueva**. Hoy la cola de `TRX_DC_DEMO` corta cada demo a los 90 frames (`phase_demo.c`) y, al acabar la cola, el título se queda esperando teclas: hace falta el gancho M13 para correrlo oculto. Variantes: en TR1, TR2 y TR3; 10 vueltas seguidas alternando (a) y (b); y una vez con el disco de tres juegos cambiando de juego entre vuelta y vuelta. | Cada vuelta llega al nivel sin caída; la demo libera su nivel al salir (el `game heap` de la carga siguiente ≈ el de una carga limpia, ± 100 K); hueco mayor y retenido estables en las 10 vueltas; la partida cargada tras una demo es la guardada (nivel correcto), y la nueva empieza en el primer nivel con el inventario inicial. |
| **P12** | **Partida real (usuario)** | `TRX.cdi` normal, sin ganchos. Guion: jugar 3 niveles de TR2 seguidos, guardar y cargar desde el pasaporte 5 veces, morir y cargar, salir al título, **dejar pasar una o dos demos, y cargar una partida (y otra vez empezar una nueva)**, cambiar a TR3, jugar 2 niveles, abrir Graphics, volver a TR2. Repetir en GDEMU cuando se pueda. | Sin caída; anotar fps y cualquier rareza. El agente solo hace el disco (el usuario prueba sus CDIs). |

Orden recomendado: 0.1 → 0.2 → P1 → P2 → P3 → P4/P5 → P8 → P6/P7 → P13 → P11
→ P9/P10 → P12. Si P1 o P2 fallan, parar y pasar a las tareas de corrección antes de
seguir.

## Tareas de corrección (según lo que falle)

| # | Si falla… | Tarea | Pistas |
|---|---|---|---|
| **M1** | P2/P4 (Barkhang, Temple) | **Bajar el pico de la carga**: las inyecciones se inflan enteras y conviven con el nivel (`game heap after the injections`, ~1,45 MB en TR3), y la conversión de texturas también. Leerlas por trozos o liberarlas antes de reservar las secciones del nivel; convertir texturas página a página. | `game/level/pipeline.c`, `game/inject.c`; medir con P4 y el censo. |
| **M2** | P2/P3 (hueco baja por carga) | **Hueco mayor que baja ~90 K por carga**: la piscina se llena en la carga y el mapa de solapes de salas (`Room_InitialiseOverlapMap`) cae cada vez más abajo. Probar: reservar ese mapa en la piscina/cabeza siempre, o con tamaño fijo máximo; medir piscina 384 vs 512 K. | `dc_level_heap.c` (`M_POOL_SIZE`, `M_LEVEL_SMALL_MAX`); T67. |
| **M3** | `brk` cerca del final (P2, P4, P9) | **Censo de los ~2,2 MB de bloques pequeños de newlib**: build con `DC_ALLOC_TRACE_MIN` 1–4 K y muestreo; agrupar con `live_census.py`. Candidatos ya vistos: cadenas del juego (dedupe hecho, medir), catálogos, configuración, tablas del flujo. | `dc_alloc_trace.c`; memoria "venice-memory-census". |
| **M4** | P2 (pico) / tiempo de carga | **Caché de transparencia de objetos** (TAREAS 66): generarla en `make_disc.sh` o guardarla en `/ram` sin directorio; evita redibujar todas las mallas en cada carga y el crecimiento del batcher en el pico. | `object_transparency_*.cache.dat`. |
| **M5** | P1/P2: retenido crece | **Fuga entre niveles**: comparar dos volcados `[kept]` consecutivos (censo) y dar con el dueño; arreglar en la descarga (`Level_Unload`, `*_ObserveLevelUnload`). | `live_census.py` con dos índices. |
| **M6** | P5 (Lud's Gate) | **Cierre de Flycast sin registro**: VRAM baja ya se descartó en parte (VQ de TR3, T65). Mirar si es un acceso fuera de RAM que Flycast no reporta: activar `TRX_DC_MMU_NULL_TRAP` (define de depuración en `src/platform/dreamcast/meson.build`) y correr con el ejecutable de depuración. | TAREAS 65 y 18. |
| **M7** | P6 (sin sonido) | **Canales de efectos tras cambiar de juego** (TAREAS 68): `Audio_Shutdown` no libera lo que `Audio_Init` reserva con `snd_sfx_chn_alloc`. Liberar en el apagado o reservar una vez por proceso. | `audio.c` de DC. |
| **M8** | P6 (287–299 K por juego) | **Lo que retiene cada juego**: medir con `live_census.py` tras el primer y el segundo cambio; si es estático (no crece) puede quedarse, documentarlo. | T70. |
| **M9** | P8 | **Reinicio de nivel sin memoria**: comprobar que el nivel viejo se libera antes de cargar el nuevo en el camino del menú (`dc_restart.lua` → recarga). | memoria "experimental-60-pending". |
| **M10** | P6/P7 sin gancho | **Ganchos de prueba que faltan**: encadenar cambios de juego y vueltas título→nivel sin teclas (para correr ocultos). Mismo estilo que `LEVELS.TXT` en `dc_main.c` y su variable en `make_disc.sh`. | |
| **M13** | P13 sin gancho | **Gancho "título → demos → partida"**: tras la cola de `TRX_DC_DEMO`, que el título lance solo una carga de partida (ranura dada) o una partida nueva, y lo repita N veces. Por ejemplo `TRX_DC_TITLE='<demos por vuelta> <frames por demo> <ranura o 0 = nueva> <vueltas>'` → `TITLE.TXT`. Hacer configurables los 90 frames fijos de `phase_demo.c` (`p->dc_frames > 90`) para ver demos más largas. El punto de entrada es `GF_DoFrontendSequence` (`game_flow/sequencer_misc.c`), que ya sirve la cola de demos; devolver `GF_START_GAME` o la acción de cargar partida cuando toque. | `dc_main.c` (lectura de `DEMO.TXT`), `make_disc.sh`. |
| **M11** | P11 | **Reservas por frame**: quitar las ~30 de `Memory_Alloc` por frame (TAREAS 9) si P11 muestra crecimiento o fragmentación. | |
| **M12** | Varias de las anteriores sin arreglo local | **Decidir la migración a arenas con época** (TAREAS 67): solo con un caso real que falle delante y sus cifras. No empezarla sin acuerdo del usuario. | TAREAS 67. |

## Reglas

- Código compartido bajo `#if defined(__DREAMCAST__)`; todo debe valer para
  TR1, TR2 y TR3.
- Nunca tocar `G:\Flycast` ni su VMU, ni matar un Flycast del usuario; pruebas
  ocultas en `G:\FlycastTest` con el audio a otro dispositivo (no silenciar
  Windows).
- Discos de prueba sin vídeos y empezando en un nivel (4º argumento).
- Decir siempre "DC build N". Nada de `build/`, CDIs, herramientas ni datos del
  juego en git.
- Un cambio que no mejore algo medible se deshace y se anota como FALLIDO.
- Al terminar cada prueba o tarea: fila en [PROBADO.md](PROBADO.md) (OK /
  FALLIDO con cifra y build), detalle y tabla de `mem_summary` en
  [REGISTRO.md](REGISTRO.md), marcar aquí la casilla o borrar la fila, y quitar
  de [TAREAS.md](TAREAS.md) las filas 64–66, 68 que queden resueltas.

## Registro de avance

(El agente apunta aquí cada prueba corrida: fecha, DC build, disco, resultado
en una línea y enlace a la sección de REGISTRO.md.)

| Fecha | Prueba | DC build | Resultado |
|---|---|---|---|
| | | | |
