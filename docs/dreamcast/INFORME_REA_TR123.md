# Informe REA: exe originales de TR1, TR2 y TR3

Fecha: 04/11/2026. Herramienta: MCP `rea` 6.8.0 con Ghidra 12.1.4. Análisis
estático solo; no se ejecutó ningún juego. Ejecutables usados (no el
remaster):

| Juego | Exe | Notas |
|---|---|---|
| TR1 | `Tomb Raider (I)\TOMBRAID\tomb.exe` (3Dfx) | DOS/4GW LE, convertido a PE con `le2flat.py` + `flat2pe.py` (entrada 0x5ec24). Direcciones = las del LE original. |
| TR2 | `Tomb Raider (II)\Tomb2.exe` | PE x86, base 0x400000. |
| TR3 | `TombRaider (III)\tomb3.exe` | PE x86, base 0x400000. |

Los nombres de funciones son interpretación nuestra (Ghidra solo da `FUN_`).
Los exe y lo extraído de ellos no entran en git; aquí solo cifras y direcciones.
Detalle por juego en el scratchpad de la sesión (`hallazgos_TR1.md`,
`hallazgos_TR1_portales.md`, `hallazgos_TR2.md`, `hallazgos_TR3.md`).

## Tabla comparativa

| Concepto | TR1 | TR2 | TR3 | TRX (referencia) |
|---|---|---|---|---|
| Bloque de memoria del juego | 0x380000 (3,5 MB) | 0x380000 | 0x380000 | – |
| Asignador | `game_malloc` 0x2fa9c, puntero que avanza | 0x44d780, ídem | 0x485f90, ídem | – |
| Reinicio por nivel | 0x2fa7c | no localizado | 0x485f60 (antes de abrir el nivel) | – |
| Huecos de IA "pesados" | **8** × 59 B | **5** × 0x42 B | **5** × 0x48 B | `LOT_SLOT_COUNT 32` |
| Huecos de IA "ligeros" (sin nodos) | no | no | **12** × 0x48 B (flag &4) | – |
| Nodos LOT por hueco | 8 B por caja | 8 B por caja | 8 B por caja | 8 B por caja |
| Tope de expansión de búsqueda | **5** | **5** | **5** | `BOX_MAX_EXPANSION 5` |
| Salas máx. al cargar | 1024 (<0x401) | 1024 | 1024 | – |
| Tamaño de sala (struct) | 0x44 | 0x50 | 0x50 | – |
| Texturas de objeto máx. | 2048 (0x800) | 2048 | **4096 (0x1000)** | – |
| Ítems máx. | 256 | 256 | 256 (0x84 B cada uno) | – |
| Id de objeto válido | 0..0xbe | 0..0x108 | < 0x178 | – |
| Efectos (fx) | no medido | no medido | 50 × 0x28 B = 2000 B | – |
| Sonidos al cargar | no medido | array de pila de 500, sin comprobar | ídem (0x4822f0) | – |
| Objetivos de disparo | – | – | array de pila de 33 | – |

## Direcciones clave

- **TR1**: `SearchLOT` 0x20890; `UpdateLOT` 0x20ab8; función que fija el
  objetivo 0x2129c (`MOV EDX,5` en 0x212ab); `LoadBoxes` 0x2db58 (zonas en
  0x10cd10/18/20, overlaps 0x10cd28, cajas 0x10cd2c a 20 B por caja);
  `LoadRoom` 0x2ce7c; texturas 0x2d290; ítems 0x2d7e0; IA 0x3b970; dibujado:
  0x28cb0 (DrawRooms), 0x28d78 (inicio), 0x28e94 (GetRoomBounds, recursiva),
  0x28f0c (clip de portal); lista visible u16 en 0x10da14, contador 0x10db18.
- **TR2**: `GameMain` 0x454b10 (`GlobalAlloc` en 0x454bac); `InitialiseLOTarray`
  0x432b10; `EnableBaddieAI` 0x432bc0; `SearchLOT` 0x40e470, `UpdateLOT`
  0x40e670, `CalculateTarget` 0x40ee50 (`PUSH 5` en 0x40ee6d); GetRoomBounds
  0x418c50 (llamada desde 0x4189a0), cola circular de 128 enteros en 0x525900,
  lista visible u16 en 0x525b20 con contador 0x5252b8; `LoadRoom` 0x449b60;
  texturas 0x449fa0; ítems 0x44a660; sonidos 0x44af50.
- **TR3**: `GameMain` 0x48c150; tamaño total en 0x634138; IA 0x452f10
  (5×0x48 + 12×0x48), `EnableBaddieAI` 0x453000 y 0x4535b0; `SearchLOT`
  0x414800, `UpdateLOT` 0x414780, `PUSH 5` en 0x4151c0; GetRoomBounds 0x4253c0
  (driver 0x424c60), cola de 128 enteros, lista visible u16 en 0x6e2220;
  carga de nivel 0x4826c0; sonidos 0x4822f0; disparo 0x44a890.

## Resultados

1. **Memoria**: los tres usan el mismo bloque fijo de 3,5 MB con un asignador
   de puntero que avanza, sin `free` por objeto (solo retroceso LIFO), que se
   reinicia en cada nivel. No hay fragmentación. En TR3 las páginas de textura
   de 16 bit se cargan en un `GlobalAlloc` temporal fuera del bloque, y las
   muestras de sonido pasan una a una por un buffer temporal (la PCM no cuenta
   en los 3,5 MB).
2. **IA**: los originales usan **8 huecos (TR1) y 5 (TR2, TR3)**; TRX usa 32.
   TR3 añade 12 huecos "ligeros" sin nodos de búsqueda para enemigos simples.
   Cuando todos están ocupados, el juego roba el hueco del enemigo más lejano.
3. **Búsqueda de rutas**: tope de **5 cajas** por llamada en los tres, igual que
   `BOX_MAX_EXPANSION`. Para criaturas voladoras TR3 ignora las zonas y usa
   una zona fija (0x2000); si no, la zona sale de una tabla indexada por la
   altura de paso (`step>>8`). Cada flipmap trae 4 tablas de zona de suelo y 1
   de vuelo (nBoxes × 2 B cada una).
4. **Salas y portales**: lista de salas visibles como array de `u16` **sin
   comprobar tope** en los tres. TR1 hace GetRoomBounds recursiva; TR2 y TR3
   usan una cola circular de 128 enteros. Los límites de pantalla salen de las
   variables de ancho y alto de ventana (no hay constantes). Capacidad real del
   array visible: **estimada en 100–112 entradas en TR1 y TR2 (no confirmada);
   sin determinar en TR3**.
5. **Topes de carga**: 1024 salas, 2048 texturas (4096 en TR3), 256 ítems,
   50 fx en TR3. El juego avisa (cadenas "Too many …") solo en salas,
   texturas e ítems.

## Sugerencias para el port

1. **Bajar `LOT_SLOT_COUNT` de 32 a 8** en DC (el máximo de los tres
   originales). Con 1000 cajas son 64 K frente a 256 K de ahora. Mejor aún,
   usarlo por versión (8 en TR1, 5 en TR2/TR3), usando `g_TRVersion`. En TR2/3
   conviene además reproducir la política de "robar el hueco del enemigo más
   lejano", que es lo que mantiene la IA barata. Medir con `gbuf_summary.py`
   y probar que ningún nivel se queda sin enemigos activos.
2. **IA ligera de TR3**: si TRX no distingue enemigos sin nodos (flag &4), estos
   enemigos pueden estar ocupando huecos pesados. Mirar cuántos son y si pueden
   usar un hueco sin array de nodos (72 B en vez de 72 B + 8 B por caja).
3. **Búsqueda**: no tocar el tope de 5; ya coincide con los originales. Si hace
   falta ahorrar CPU, es el único parámetro que fija el coste por fotograma, así
   que bajarlo a 3–4 sería probar contra el comportamiento original.
4. **Memoria**: los originales caben en 3,5 MB porque todo vive en un bloque que
   se reinicia por nivel y la PCM de sonido se carga por demanda. Es el modelo
   a imitar en DC: un único bloque por nivel, y no mantener la PCM completa
   residente. Cualquier reserva por objeto que no se pueda devolver en bloque
   es la que fragmenta.
5. **Lista de salas visibles**: fijar un tope explícito de **128** entradas (la
   misma capacidad que la cola de TR2/TR3) con comprobación. Los originales no
   comprueban nada, así que un nivel grande hoy desborda sin avisar.
6. **Robustez de carga**: los originales tienen arrays de pila sin comprobar
   (500 sonidos al cargar, 33 objetivos de disparo). En TRX/DC añadir
   comprobaciones baratas (`assert`/clamp) donde el nivel venga de un archivo
   que no controlamos.
7. **Texturas**: TR3 permite 4096 texturas de objeto (20 B cada una = 80 KB de
   tabla) frente a 2048 de TR1/TR2. Comprobar que el array de DC admite 4096.
8. **Efectos**: el array de fx de TR3 es de solo 50 × 40 B. Si TRX reserva
   bastante más, es un ahorro seguro de RAM, aunque no medido: faltan
   partículas, splash, ripples, blood, bubbles y gunflash.

## Qué no está confirmado

- Reinicio del puntero del asignador en TR2.
- Tamaño real del array de salas visibles (TR1/TR2 estimado; TR3 sin determinar).
- Pila de matrices de GetRoomBounds en TR1; luces, sombras y objetos por sala.
- Arrays de partículas y efectos en TR1/TR2; tope de objetos activos y de
  cajas; tamaño de la tabla de páginas de textura de TR3.
- TR1 viene de una conversión LE→PE; las referencias de puntero lejano de 16
  bits pueden estar mal en el desensamblado.
- Todo es estático: no se comprobó en ejecución ninguna de estas cifras.
