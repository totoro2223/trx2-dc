# REA sobre los exe originales de TR1, TR2 y TR3

Objetivo: entender cómo los juegos originales caben en 8 MB y mueven la IA con
poca CPU, y sacar las cifras que sirvan al port (memoria, huecos de IA,
búsqueda de rutas; después salas y portales). Lo que aplique al port pasa a
TAREAS.md como tarea normal; antes, mirar PROBADO.md.

Abierto el 04/11/2026. Herramienta: el MCP `rea` (Ghidra 12.1.4 detrás).

**HECHO el 04/11/2026**: resultados de los tres exe en
[INFORME_REA_TR123.md](INFORME_REA_TR123.md). Las tareas de abajo se ejecutaron;
lo que no se resolvió está en la tarea 80 de TAREAS.md y la aplicación al port
en la 79. Este fichero queda como registro del método (cómo abrir cada exe).

## Cómo abrir cada exe

- **TR1** (DOS/4GW, formato LE, que REA no admite):
  `H:\SteamLibrary\steamapps\common\Tomb Raider (I)\TOMBRAID\tomb.exe`
  (versión 3Dfx; `TOMBno3Dfx.EXE` es la de software). En Windows, Ghidra de
  REA solo acepta PE, así que se convierte:

  ```bash
  python -I tools/dreamcast/le2flat.py "<tomb.exe>" <tmp>/tomb_flat.bin
  python -I tools/dreamcast/flat2pe.py <tmp>/tomb_flat.bin <tmp>/tomb_le.exe 0x5ec24
  ```

  Después `open_binary` con `<tmp>/tomb_le.exe`. Las direcciones son las del
  LE original (código 0x10000–0xb1000, datos desde 0xc0000). Las fixups de
  puntero lejano quedan sin selector: alguna referencia de 16 bits puede salir
  mal. Watcom pasa los argumentos en registros (EAX, EDX, EBX, ECX): cuando el
  pseudocódigo pierde un argumento, mirar `procedure_assembly`.
- **TR2**: `G:\SteamLibrary\steamapps\common\Tomb Raider (II)\Tomb2.exe` (PE).
- **TR3**: `G:\SteamLibrary\steamapps\common\TombRaider (III)\tomb3.exe` (PE).
  TR2 y TR3 deberían abrirse tal cual; sin comprobar si la versión de Steam
  lleva parche o envoltorio.

Al terminar, `close_binary`. Los exe y lo que se saque de ellos no entran en
git (copyright); aquí solo cifras y direcciones.

## Hecho en TR1 (04/11/2026)

- **Memoria**: un bloque fijo de **3,5 MB** (`MOV ECX, 0x380000` en
  `0x2f9a0`). `game_malloc` (`0x2fa9c`) solo avanza un puntero (alineado a 4)
  y, si no cabe, sale con `"game_malloc(): OUT OF MEMORY %s %d"`. No hay
  `free` por objeto: `0x2fa7c` vuelve el puntero al principio en cada nivel,
  así que no hay fragmentación. Todo el `.PHD` se carga en ese bloque; no hay
  caché de salas. Nombres de categoría en la tabla `0x10346c`.
- **Huecos de IA**: **8** (`0x3b970`: 8 × 59 bytes, `"Creature Data"`, más un
  array de 8 bytes por caja en cada hueco, `"Creature LOT"`). TRX usa
  `LOT_SLOT_COUNT 32`.
- **Rutas**: cajas + overlaps + zonas (`ground_zone`, `ground2_zone`,
  `fly_zone`, cada una con su versión de flipmap) vienen en el nivel. La
  búsqueda es hacia atrás desde el objetivo, repartida por fotogramas; el
  tope de 5 cajas por fotograma sale de TRX (`BOX_MAX_EXPANSION`,
  `src/trx/game/pathing/box.c`), **no localizado aún en el exe**.

## Tareas

### REA TR1

1. Localizar en el exe el tope de expansión de la búsqueda (5 en TRX):
   buscar `SearchLOT` por sus constantes `0x7fff`/`0x8000` o a partir de las
   referencias a las zonas que carga `LoadBoxes` (`0x2dcf4`).
2. Salas y portales: cómo decide qué salas dibuja (límites de pantalla por
   portal, número máximo de salas por fotograma) y si hay algún tope fijo.

### REA TR2

1. Tamaño del bloque de memoria del juego y cómo se pide (¿mismo
   asignador de puntero que avanza?).
2. Número de huecos de IA (equivalente a `0x3b970` de TR1) y tope de
   expansión de la búsqueda.

### REA TR3 (el peor caso: hacerlo a fondo)

1. Bloque de memoria y asignador, como en TR2.
2. Huecos de IA y tope de expansión; qué cambia en la búsqueda (zona fija de
   los voladores, `g_TRVersion == 3` en `box.c`).
3. Salas y portales, como en TR1.
4. Cualquier otro tope fijo que explique cómo cabe en la memoria (sonidos,
   texturas, objetos activos).

### Después de las tres

Con el mayor número de huecos de los tres originales, valorar bajar
`LOT_SLOT_COUNT` en DC (cada hueco cuesta 8 bytes por caja del nivel; con
1000 cajas, 32 huecos son 256 K frente a 64 K con 8). No figura en
PROBADO.md. Medir con `gbuf_summary.py` y probar que ningún nivel de TR1–TR3
se queda sin enemigos.

**Estado (09/11/2026):** probado como "Enemy AI: Original" (8/5 huecos) y retirado el 04/11 porque las demos no acababan en su sitio; hoy `LOT_SLOT_COUNT` es 32 siempre, también en Original (`lot.c`). Ver INFORME_REA_TR123.md y TAREAS 79.
