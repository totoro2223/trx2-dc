# Reporte: memoria y sesiones largas

Seguimiento de la ejecución del
[plan de memoria en sesiones largas](PLAN_MEMORIA_SESIONES_LARGAS.md) hecha sin
asistencia (Fase 0 y toda la batería que no necesite hardware ni teclas).

Fecha: 2026-10-26. Repo: `TRX`, rama `dreamcast`. Build de referencia:
`build/dc-minsize` (`DC build 805`, `minsize`). La línea base de P1 se midió
con los builds 765–771 y la de P2 con 772–774; las cifras de abajo que no dicen
otra cosa son de esos builds. Los logs serie de toda la tanda están en
`build/logs/2026-10-26/` (con un `INDICE.txt` que dice qué prueba es cada uno).

## Resumen

| Fase | Estado | Resultado |
|---|---|---|
| 0.1 Compilar | OK | `minsize` compila sin errores (ELF de build 805) |
| 0.2 `mem_summary.py` | OK | lee los logs y saca una tabla por carga |
| 0.3 Línea base P1 | **OK** | TR1, TR2 y TR3 llegan a `[levels] done` (TR3 en el build 784) |
| 0.3 Línea base P2 | **TR3 pendiente** | TR1 (build 795) y TR2 (build 794) pasan; en TR3 el arreglo de la carga está probado en 5 niveles (build 805) |
| P1 Niveles seguidos | **OK** | los tres juegos (detalle abajo) |
| P2 Niveles + guardados | **TR1/TR2 OK, TR3 pendiente** | ver abajo |
| P3 Cargas repetidas | pendiente | — |
| P4 Pico de Barkhang | pendiente | la carga de Barkhang ya se midió (6240 K de 6591 K); falta el pico con guardados |
| P5 TR3 entero | pendiente | es la pasada larga de P2 TR3 |
| P8 Reiniciar / cambiar modo | pendiente | — |
| P11 Sesión larga | pendiente | — |
| P6/P7/P13 | no ejecutable aún | necesitan ganchos M10/M13 o teclas |
| P9/P10/P12 | no ejecutable | necesitan teclas, hardware o tarjeta llena |

## Verificación previa

- Árbol limpio; el trabajo anterior está en el commit `1cd173746` (build
  estricto, migas e informe de cuelgue, auditorías PVR, bloques OPB de 32).
- `build/dc-minsize/TRX.elf` reconstruido desde ese commit.
- No había ningún Flycast del usuario abierto antes de cada prueba.
- Datos: `H:\GOG\Tomb Raider I-III Remastered\{1,2,3}\DATA`.
- Emulador de pruebas: `G:\FlycastTest\flycast.exe`; tarjeta de pruebas
  `build\vmu_test_card.bin`.

## Fase 0.2: lector de logs

`tools/dreamcast/mem_summary.py` resume un log serie de TRX en Markdown:

```powershell
py tools\dreamcast\mem_summary.py build\dc-serial.txt -o build\mem-a.md
```

Saca una fila por carga con nivel, `game heap`, hueco mayor, bloques, pico,
inyecciones, `spilled`, Lua, `brk` y su distancia al final de la RAM, VRAM
libre, lo retenido (`[kept]`), el hueco de newlib (`[heap]`), las cargas de
partida en serie (`[test] load`), las auditorías PVR (`[bins]`/`[audit]`) y la
primera línea de fallo. Los rechazos de `sbrk` de newlib (que reintenta con
menos memoria hasta que entra) se cuentan aparte y no como fallo.

## Resultados

### P1: niveles seguidos, sin guardar

Discos **sin películas** (`TRX_DC_FMV=0`, como pide el plan), 300 fotogramas por
nivel. Logs en `build/logs/2026-10-26/`; resumen generado con
`py tools\dreamcast\mem_summary.py`.

| Juego | Disco | Niveles | Resultado |
|---|---|---|---|
| TR1 | `T-mem-P1-tr1.cdi`, DC build 769 | 0–15 (16) | **Pasa**: `[levels] done` |
| TR2 | `T-mem-P1-tr2.cdi`, DC build 770 | 0–17 (18) | **Pasa**: `[levels] done`; Barkhang (12) carga sin guardados |
| TR3 | `T-mem-P1-tr3.cdi`, DC build 771 | 0–17 cargados | **Falló al cargar 18 (`mines.tr2`)**: `Out of memory`, `realloc 400K` falla con `brk` a 384 K del final |
| TR3 (repetición) | `T-mem-P1-tr3f.cdi`, DC build 784 | 0–18 (19) | **Pasa**: `[levels] done` |

Cifras por juego (región del nivel en `game heap`):

| Juego | Región | Pico máximo | Hueco mayor mínimo | Retenido | Lua spilled | `brk` mínimo | VRAM libre |
|---|---:|---:|---:|---:|---:|---:|---:|
| TR1 | 6655 K | 5778 K (87 %) | 5460 K → 6271 K al final | 3 bloques, 0 K | 0 | 528 K | 3882–4405 K |
| TR2 | 6591 K | 6240 K (95 %) | 6011 K | 6 bloques, 0 K | 0 | 388 K | 4220–4405 K |
| TR3 | 6591 K | 6030 K (91 %) | 5618 K | 11 bloques, 0 K | 0 | 496 K | 3350–4405 K |

- El hueco mayor baja en los primeros niveles y **se recupera** al final (TR1 y
  TR2). El desvío mayor es TR1 (811 K) y coincide con las cargas con más
  inyecciones; no hay tendencia monótona.
- El pico de TR2 (Barkhang incluido) queda en 6240 K de 6591 K: **por debajo
  del 95 %** del criterio de P4, aunque justo.
- TR3 no llegaba a 18 en el build 771. La caída era de memoria de newlib, no de
  la región: la región estaba a 6591 K y `brk` dejaba 384 K; el siguiente nivel
  pedía 400 K mientras leía inyecciones (`Inject_AllInjections` → texturas). La
  última miga era `7 first frame` del nivel 17 (`antarc`). Lo arreglaron tres
  cosas (builds 775–784): la región se prueba **antes** que newlib en cada
  reserva, un `realloc` grande que newlib no puede crecer se **mueve entero a la
  región** (en TR3 las minas tenían 4,6 MB libres allí), y si aun así no hay
  sitio se tiran las mallas de objetos no dibujadas y la basura de Lua y se
  reintenta. La repetición (build 784) hace los 19 niveles con `[levels] done`.
- `spilled` de Lua se queda en 0 en los tres. Los bloques pequeños retenidos no
  suman ni 1 K.

### P2: niveles seguidos con guardados y cargas

Discos **sin películas**, `TRX_DC_SAVE=90` (un guardado o una carga cada 90
fotogramas, por turnos en las ranuras 1–5) y 300 fotogramas por nivel, desde el
nivel 0. Tarjeta de pruebas recién copiada de `build\vmu_test_card.bin` en cada
pasada. Logs en `build/logs/2026-10-26/` (`dc-mem-P2-trN.txt`; `-tr2b` es la
repetición del fallo de TR2, idéntica).

| Juego | Disco | Build | Niveles | Guardados / cargas | Resultado |
|---|---|---|---:|---:|---|
| TR1 | `T-mem-P2-tr1.cdi` | 772 | 0–15 (16) | 45 / 8 | **Pasa**: `[levels] done`, 0 «could not be copied» |
| TR2 | `T-mem-P2-tr2.cdi` | 773 | 0–11 | 40 / 8 | **Falló** al guardar en Barkhang (`monastry`, 12) |
| TR3 | `T-mem-P2-tr3.cdi` | 774 | 0–2 | 11 / 2 | **Falló**: el proceso de Flycast desaparece cargando el 3, sin mensaje |
| TR2 (repetición) | `T-mem-P2-tr2k.cdi` | 794 | 0–16 (17) | — | **Pasa**: `[levels] done`; el nivel 16 guarda con 6020 K de 6591 K, BSON de 196 K y 34 bloques |
| TR1 (repetición) | `T-mem-P2-tr1b.cdi` | 795 | 0–15 (16) | 45 / 8 | **Pasa**: `[levels] done` |
| TR3 (corto, 11→12) | `T-mem-tr3-short2.cdi` | 798 | 11–12 | 3 / — | **Pasa**; BSON de 244 K, arena toda de la región |
| TR3 (5 niveles) | `T-mem-tr3-l5e.cdi` | 805 | 0–4 (5) | 17 / 3 | **Pasa**: `[levels] done`; carga en 6 trozos, sin petición de 700 K |
| TR3 (pasada completa) | `T-mem-P2-tr3e.cdi` | 804 | 0–18 (19) | — | **Pendiente**: disco hecho, sin ejecutar |

Cifras:

| Juego | Región | Pico máximo | Hueco mayor mín. | `spilled` máx. | `brk` mín. | VRAM libre |
|---|---:|---:|---:|---:|---:|---:|
| TR1 | 6655 K | 5867 K (88 %) | 5457 K | 0 | 364 K | 3882–4405 K |
| TR2 | 6591 K | **6591 K (100 %)** | 6112 K | **110 518** | **76 K** | 4198–4405 K |
| TR3 | — | — | — | — | — | — |

**TR2, Barkhang (12):** el nivel llena la región entera (pico = 6591 K, 100 %) y
empuja 110 K de Lua a la memoria de newlib (`spilled`). Al guardar
(`test: save 40`), el guardado pide dos bloques de 175 K —el BSON sin comprimir y
su copia comprimida, `savegame/file.c`—; el primero entra y el segundo no:

```
[alloc] malloc 175K -> 0x8cfa65d0 from 0x8c021e22 (in use 12607K)
Out of memory. Requested sbrk_base 0x8d009000, was 0x8cfed000, diff 114688
[alloc] malloc 175K -> 0x0 from 0x8c021e22
ERR | core/memory.c:75:Memory_Alloc] Assertion failed: result != nullptr
arch: aborting the system
```

El guardado ya entra en la región del nivel (`DC_LevelHeap_Enter` en
`SG_File_SaveToFile`, puesto para Jungle), así que lo que falla es que la región
**no tiene ningún hueco de 175 K**: está ocupada por el nivel y por las
inyecciones que siguen vivas. En P1 el mismo nivel pica 6240 K (95 %) sin
`spilled` y con `brk` a 484 K: los guardados suben el pico ~350 K y son la
diferencia entre pasar y no pasar.

La repetición (`-tr2b`) da el mismo fotograma y el mismo fallo: es determinista.

**Arreglo de TR2 (build 794):** el guardado en Dreamcast ya no pide el segundo
bloque. zlib escribe el comprimido **por trozos de 16 K** según lo produce y las
tallas de la cabecera se parchean al final (`savegame/file.c`, `M_SaveBSON`; el
camino de escritorio no cambia). Además el árbol del guardado se construye
**sección a sección** en una arena de trozos (`dc_save_arena.c`): de cada
sección solo viven su árbol y su BSON, no el árbol del estado entero, y los
trozos se reutilizan entre secciones. En la repetición el nivel 12 guarda con la
región a 6020 K de 6591 K (hueco mayor 556 K) y el BSON de 196 K ocupa 34
bloques de VMU, las mismas tallas que antes de trocear. TR1 repite con 45
guardados y 8 cargas sin cambios.

**TR3:** cae mucho antes que en P1 (que llegaba al 18) y **sin mensaje**. Dos
pasadas:

| Pasada | Duración | Última operación | Final |
|---|---:|---|---|
| `dc-mem-P2-tr3.txt` | 2 min | `test: load 11`, inyecciones del nivel 3 leídas | se corta en `Level_Format_GuessLoader` |
| `dc-mem-P2-tr3b.txt` | 1 min | `test: save 1` copiado a la VMU (19 bloques) | se corta 2 fotogramas después, sin nada |

No aparece `Assertion`, ni `Out of memory`, ni `TRX: exception…`, ni `TRX
STALLED`, ni una miga de cuelgue; el `[watchdog]` anterior muestra el hilo
principal en código normal del nivel. El proceso de Flycast desaparece sin
escribir **ni una línea** en `flycast.log` (los dos, tras un guardado o una
carga de la VMU), que es lo que distingue esto de una caída del juego: una
excepción o un `arch_abort()` del juego sí dejan rastro en el serie (se vio en
TR2). No es determinista (unos 20 minutos de partida distintos); queda como
observación aparte, ver «Fallos y ajustes».

**Arreglo de TR3: leer el árbol en trozos (builds 800–805).** Los repros
posteriores sí dejaron aserto, y siempre en el mismo punto: al **aplicar** la
partida tras cargar un nivel, `BSON_ParseEx` pedía el árbol entero de una vez.
Un BSON de 154 K pedía **713–718 K** (unas 4,6 veces), con la región a 5951 K de
6591 K y el hueco mayor en 606 K, y newlib con 788 K libres pero solo 65 K en la
cima:

```
[big] 713K: region 5951K of 6591K, largest 606K, spilled 16685; newlib free 788K, top 65K
Assertion failed: result != nullptr
```

Era lotería de reparto: los mismos 5 niveles entraban en el build 801 y no en el
802. Ahora el árbol se lee **en trozos** con la misma arena del guardado: los
nodos y las cadenas se reservan uno a uno en trozos de la región (que `M_Grow`
parte por la mitad hasta que uno entra) y, si no, de newlib. El árbol de 154 K
son ~67 000 nodos en 6 trozos: 448 K de la región y 484–496 K de newlib, y al
terminar la carga se devuelven enteros. La comprobación de 5 niveles (build 805)
llega a `[levels] done` con 17 guardados y 3 cargas, y ya no hay ninguna
petición de 700 K.

`BSON_Parse` solo usa la arena cuando hay un guardado abierto
(`DC_SaveArena_IsOpen`): cualquier otro BSON, y todo el camino de escritorio,
sigue reservando el bloque único de antes.

### Auditoría PVR de las mismas pasadas

`[bins]` con bloques OP 32, TR 16, PT 32 (el cambio del commit `1cd173746`),
máximos sobre **todas** las muestras (182, 205 y 180):

| Juego | OP máx / teselas sobre | TR máx / teselas sobre | PT máx / teselas sobre |
|---|---|---|---|
| TR1 | 203 / 8 | 33 / 24 | 142 / 9 |
| TR2 | 161 / 7 | 30 / 24 | 172 / 10 |
| TR3 | 161 / 27 | 45 / 46 | 98 / 7 |

- **Los bloques de 32 no alcanzan**: hay teselas con hasta 203 punteros (OP) y
  172 (PT) en un fotograma. TR sigue con bloque de 16 y llega a 45.
- Vértices: 0 no finitos y 0 sin profundidad en los tres; los "fuera de
  pantalla" son polígonos grandes recortados (máximo 1173 en TR3).
- No se puede juzgar aquí si el desborde se ve en pantalla; el aviso de la
  auditoría es nuevo y esto es la primera medida con OPB 32.

### Películas en los discos de prueba

El primer intento (DC builds 766–768) llevaba todas las películas. En TR1 el
gimnasio (`gym.phd`) hace `play_fmv(3)` y el juego **se queda parado** en el
fotograma 114: el `[watchdog]` muestra el hilo principal dentro del reparto de
eventos de Lua (`luaB_next`/`lua_settop`), sin volver al bucle. Sin películas,
los 16 niveles pasan. Por eso las pruebas siguen el plan (`TRX_DC_FMV=0`) y
esto queda como aviso aparte: no es un problema de memoria, pero un disco con
vídeos que empiece en el gimnasio se cuelga.

## Fallos y ajustes

- **P1 TR3 (arreglado, build 784):** OOM al cargar `mines.tr2` con `brk` a
  384 K. Lo arreglaron tres cosas: la región se prueba **antes** que newlib en
  cada reserva, un `realloc` grande que newlib no puede crecer se **mueve entero
  a la región** y, si aun así no hay sitio, se tiran las mallas de objetos no
  dibujadas y la basura de Lua y se reintenta. Los 19 niveles pasan.
- **P2 TR2, el guardado (arreglado y medido, builds 794–795):** `M_SaveRaw`
  pedía dos bloques del tamaño del BSON (175 K el BSON y 175 K su copia
  comprimida) y la región del nivel, llena en Barkhang, no tiene ese hueco. En
  Dreamcast el comprimido se escribe **por trozos de 16 K según lo produce
  zlib** y el árbol se construye sección a sección (`dc_save_arena.c`,
  `savegame/file.c`; el camino de escritorio queda igual). TR2 hace 17 niveles y
  TR1 repite con 45 guardados.
- **P2 TR3, aplicar la partida (arreglado en corto, builds 800–805):** el árbol
  del BSON se pedía entero (713–718 K). Ahora se lee en trozos con la arena del
  guardado; el árbol de 154 K son 6 trozos de 448 K de la región y 484–496 K de
  newlib. Los 5 niveles de comprobación pasan. **Falta la pasada de 19 niveles**
  (`T-mem-P2-tr3e.cdi`, build 804) para dar P2 TR3 por cerrado.
- **TR3 se caía sin mensaje (build 774):** las dos caídas de la primera pasada
  no dejaban nada en el serie ni en `flycast.log`. Los repros con más detalle
  dieron aserto en `BSON_ParseEx` en el mismo punto (arriba), que encaja; las
  pasadas cortas con el arreglo no lo reproducen. Si vuelve en la pasada larga,
  descartar puntero salvaje con `TRX_DC_MMU_NULL_TRAP` (M6) y mirar el lado del
  emulador (las dos caídas fueron junto a una escritura/lectura de la VMU).
- **Auditoría PVR:** OPB 32 no evita el desborde; TR sigue en 16. Pendiente de
  decidir si se sube TR a 32 y si OP/PT necesitan más de 32 o repartir por
  franjas.

## Estado y pendiente

Hecho en esta tanda (sin hardware, sin teclas):

- Fase 0 completa: `minsize` compila y `mem_summary.py` resume los logs.
- **P1** OK en TR1, TR2 y TR3 (TR3 en el build 784).
- **P2** OK en TR1 (build 795) y TR2 (build 794); TR3 probado en corto y en 5
  niveles (build 805), con el arreglo del guardado y de la carga.
- Tres arreglos en el camino: región antes que newlib + `realloc` a la región +
  reintento con desalojo (P1 TR3); guardado por trozos y por secciones (P2 TR2);
  árbol BSON leído en trozos con la arena (P2 TR3).

Pendiente, en este orden:

1. **P2 TR3 completo**: `T-mem-P2-tr3e.cdi` (build 804), 19 niveles con
   `TRX_DC_SAVE=90`; es también P5. Confirmar que no vuelve la desaparición.
2. **P3** cargas repetidas, **P4** pico de Barkhang con guardados, **P8**
   reiniciar/cambiar de juego, **P11** sesión larga (todo en Flycast).
3. **Quitar las sondas `[save]` y `[arena]`** (TEMP) cuando la batería cierre;
   son la evidencia que queda en los logs.
4. Con hardware/teclas/ojos del usuario: **P6, P7, P9, P10, P12** (consola y
   GDEMU), **P13** y la decisión sobre **M12**.

