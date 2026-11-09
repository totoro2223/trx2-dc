# REA del TR1 de PSX frente al port de Dreamcast

Abierto el 05/11/2026. Solo direcciones y cifras: el volcado no entra en git
(copyright). Estimaciones **estáticas** (instrucciones contadas a mano en el
desensamblado), sin ejecutar el PSX: valen para el orden de magnitud.

## Cómo se abrió

- ISO `D:\trx\Tomb Raider (v1.6)` (NTSC-U, `SLUS_001.52`). El juego es
  **`TOMBRAID.EXE`** (501 760 B; `MAIN.EXE` es el menú). Carga en
  **0x80010000** (KSEG0), `gp` = **0x80087530**, entrada 0x800603dc.
- Ghidra 12.1.4 headless sobre el binario sin cabecera (`MIPS:LE:32`,
  `BinaryLoader`, base 0x80010000). Volcado: `D:\trx\_rea\tr1psx\functions`
  (1186 funciones); `scripts/DumpAll.java`; desensamblado con capstone
  (`skipdata`, las órdenes GTE salen como `GTE cmd 0x…`).

## Dibujo de sala (0x80010000, llamado desde 0x800501e8)

Va **por cara, sin vértices compartidos**: por cada cuadrilátero (5 u16:
4 índices + textura) hace RTPT de 3 vértices (0x280030, 23 ciclos), NCLIP
(0x1400006, 8), RTPS del cuarto (0x180001, 15), recorte de pantalla con
máscaras (4 vértices contra 4 bordes), máximo de z para la OT (z>>3) y
descarte a z > 0x5000. No hay iluminación en el bucle: el color de vértice
viene ya en los datos (los 4 colores se leen de la tabla de caras, 16 B).

Por distancia (z máximo de la cara):

| z | Qué hace |
|---|---|
| ≥ 0x1000 (4 casillas) | una sola cara; niebla desde 0x3000 (`(z-0x3000)>>5` restado al color) |
| 0xaaa-0x1000 | **subdivide 1 vez** (0x800108bc recursiva, `s7`=1): 4 caras |
| < 0xaaa | **subdivide 2 veces**: hasta 16 caras (corrige la textura afín del PSX) |

0x80010fb4 es la misma lógica para otro modo (textura de otro tipo).
Mallas de objeto: 0x800530b8, 0x80050d10, 0x80051db0, 0x800528a0 (con normales
y luz por cara, RTPT + 3 × NCS/NCCS).

## Cuenta de ciclos por polígono (PSX, R3000 33,8 MHz, ~1 ciclo/instrucción)

Cara lejana (≥ 0x1000), camino típico visible y sin niebla:

- 106 instrucciones hasta la decisión de distancia (carga de índices,
  direcciones de vértice, RTPT, NCLIP, RTPS, máscaras de recorte, z)
- ~47 de color/niebla + 32 de emisión (13 `sw` de la primitiva de 52 B)
- ≈ 185 instrucciones; la GTE casi se solapa con la CPU (RTPT se lee 30
  instrucciones después, RTPS 16): stalls ≈ 10
- lecturas de vértice (8 `lw/lwc2` de RAM, sin D-cache útil) ≈ 40 y escrituras
  ≈ 10
- **≈ 250 ciclos = 7,4 µs por cuadrilátero lejano**

Cara media: 250 + 4 × ~110 ≈ **650**; cara cercana (< 0xaaa): **~2000+**
(hasta 16 subcaras). El PSX gasta *más* en lo cercano que lo lejano.

## Port (Vilcabamba, bench 0, Flycast, build con sondas)

Medias por fotograma: 723 polígonos, 5895 vértices; `pass` 8,2 ms,
`xform` 4,1 ms, `stage` 3,4 ms, `logic` 1,8 ms, `submit` 1,2 ms.

| | ciclos/polígono | µs/polígono |
|---|---|---|
| PSX cara lejana (estimado) | ~250 | 7,4 |
| PSX incluyendo subdivisión (mezcla, estimado) | ~250-2000 | 7-60 |
| Port, solo `pass` (Flycast, 200 MHz) | ~2270 | 11,3 |
| Port, `logic+stage+pass+submit` | ~4040 | 20 |

Lectura: por polígono lejano el port gasta **~9× más ciclos** y ~1,5× más
tiempo que el PSX, pese a 6× de reloj. El PSX paga la subdivisión en lo
cercano (el port no la necesita: la PVR corrige la perspectiva), así que la
comparación justa es con la mezcla, no con 250. Flycast cuenta ~2 ciclos por
instrucción y sin caché: en consola el port da 17-18 fps (~55 ms por
fotograma), 3-4× lo medido aquí.

## Qué se ve que se puede copiar

1. Un vértice compartido se transforma **una vez** en el port (xform 4,1 ms
   con 5895 vértices ≈ 0,7 µs/vértice = 140 ciclos) frente a 4 por cara en el
   PSX: el coste por vértice del port es el problema, no el número.
2. El PSX lee 8 B/vértice (x,y,z,pad + color) y escribe 52 B/primitiva; el
   port lee 28 y escribe 32 por vértice: ancho de banda ~3,5× mayor.
3. El descarte por pantalla del PSX son 4 máscaras (`and` de restas) sin
   ramas; mismo criterio que `dc_xform_room`.

Estado (09/11/2026): 1 hecho (queda la copia de compartidos en `xform`,
tarea 83); 2: store queues y `pref` sin ganancia en consola (Jungle, DC build
1219/1221), el formato compacto sin hacer; 3 ya es el criterio del port.

## Enemigos (PSX frente al port)

PSX: el bucle por tick (0x8001ec30) recorre la lista de ítems **activos**
(0x48 B cada uno) y llama a `control` por objeto (tabla 0x8008f78c, paso 0x34,
la rellena 0x80045608); después los efectos, Lara y la cámara. Sin saltos de
ticks ni IA espaciada: cada enemigo corre su IA entera en cada tick. La IA es
la del PC: info de IA (0x80015dfc), estado de ánimo (0x800168bc), búsqueda de
caminos con **5 nodos por llamada** (0x80016150, desde 0x8001783c) y el
movimiento/colisión del enemigo (0x800181a4, 4628 B). La colisión común de
todos los enemigos es 0x8001da44.

Port (demo 1 de TR1, sondas, Flycast; 17 → 83 instancias al aparecer enemigos):
`items` (lógica) 0,03 → 0,3 ms; `sitem` (dibujar los ítems en la etapa)
0,05 → **2,6 ms**; `vobj` 1,3 → 3,1 ms; `emit` 0,9 → 3,3 ms; `pass`
2,7 → 8,9 ms. El coste de los enemigos está en **dibujarlos**, no en la IA:
~40 µs de etapa por instancia (matrices por hueso, interpolación de
fotogramas, `Output_CalculateObjectLighting`, sombra) más sus vértices.
