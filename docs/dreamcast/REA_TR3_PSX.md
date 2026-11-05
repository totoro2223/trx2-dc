# REA del TR3 de PSX frente al port de Dreamcast

Abierto el 05/11/2026. El PSX (NTSC-U, v1.2, `SLUS_006.91`) mueve TR3 a ~30 fps
con una CPU de 33 MHz; el port en DC da 15-20 fps en consola con la
configuración visual del PSX. Aquí, lo que hace el original y qué se puede
sacar. Solo direcciones y cifras: el volcado no entra en git (copyright).

## Cómo se abrió

- `SLUS_006.91` (24 KB) es el cargador. `MAIN.EXE` es el menú/legales/FMV.
  **El juego es `TOMB3.EXE`** (563 200 bytes tras la cabecera PS-X de 0x800).
- REA no abre MIPS ni PS-X EXE: se usó Ghidra 12.1.4 directo
  (`analyzeHeadless`, `MIPS:LE:32:default`) sobre el binario sin cabecera,
  **base 0x10000** (no 0x80010000: el código usa direcciones KUSEG) y
  **`gp` = 0x961a0** (`lui gp,9; addiu gp,0x61a0` en 0x76688); sin `gp` los
  globales salen como `unaff_gp`.
- Extracción del BIN: `D:\trx\_rea\tr3psx\isoextract.py` (ISO9660 MODE2/2352).
  Volcado: `D:\trx\_rea\tr3psx\functions` (1403 funciones, `0x000xxxxx.c`),
  índice `index.txt`; script `scripts/DumpAll.java`, grafo de llamadas
  `scripts/cg.py`. Las semillas de función son los destinos de `jal` y los
  prólogos `addiu sp,sp,-N`: puede faltar alguna función de tabla de saltos.

## Mapa (direcciones del PSX)

| Qué | Dónde |
|---|---|
| Arranque, bucle de niveles | 0x10020 |
| Bucle de juego (`while(!update(n)) n = draw+wait`) | 0x6d07c |
| Lógica de N ticks (tope 10 por llamada) | 0x1ac28 |
| Dibujo de un fotograma | 0x29a98 |
| Recorrido de portales y ventanas por sala | 0x8a6e0 |
| Sala: por trozos, luces dinámicas, niebla | 0x88e9c / 0x884f8 |
| Estáticos e ítems de cada sala | 0x2967c, 0x89da4 (caja), 0x2a4d8 |
| Malla de objeto (Lara, enemigos, estáticos) | 0x8c2d8 |
| Sprites / efectos | 0x73560, 0x74140, 0x74c28 |
| Espera de fotograma (DrawSync, VSync ≥ 2, intercambio de búferes) | 0x647bc |
| Inicio de fotograma (OT y búfer de primitivas) | 0x646bc |

## Lo que hace el original (verificado en el código)

1. **Presupuesto duro de polígonos**: el búfer de primitivas mide 0x19438
   bytes = **~1990 polígonos de 52 bytes**; el contador se imprime como
   `Polys %d/%d/2000`. Cada emisión comprueba `ptr < fin` y, si está lleno,
   **deja de añadir** (no hay segunda pasada ni aviso). Tabla de orden (OT) de
   0xa02 entradas, profundidad = z>>3 (z máx. 0x5000).
2. **Pantalla 512×240** (recorte a 0x1ff × 0xef), doble búfer, CPU y GPU
   solapadas (`DrawSync(0)` al final, antes de esperar el VSync).
3. **Mismo reloj que el port**: lógica a 30 Hz, tope de 10 ticks por vuelta,
   dibuja y espera ≥ 2 VSync. No hay interpolación ni 60 fps.
4. **Distancia**: niebla desde z = 0x3000 (12 casillas) y todo recortado en
   0x5000 (20). El port ya usa 10 / 18 en TR3 (más corto que el original).
   Los sprites y los ítems se descartan antes de nada con
   |dx|,|dy|,|dz| < 0x5000.
5. **Salas por trozos de ≤ 127 vértices** (índices de 7 bits, vértice y cara
   de 4 bytes cada uno, color de 5+5+5 bits). Cada trozo: caja de 8 esquinas
   (3 × RTPT) contra la **ventana del portal**; fuera = no se toca; dentro
   entero = bucle sin recortar caras. La iluminación de cada vértice sale ya
   horneada; las luces dinámicas solo se aplican a los vértices marcados
   (lista de luces del trozo en la memoria rápida 0x1f800300, atenuación por
   tabla).
6. **Caras**: NCLIP (cara trasera) + profundidad media/máxima. En las lejanas
   (z > 0x1800) **la cara trasera siempre se descarta**; además la cara usa
   uno de **tres juegos de UV según la distancia** (0x2000 / 0x4000): es un
   mipmap por cara.
7. **Mallas de objeto** (0x8c2d8): ≤ 105 vértices por malla, transformados de
   tres en tres (RTPT). **La niebla se calcula una vez por malla** (con la z
   del origen, `(z-0x3000)>>1`) y no por vértice. Las mallas con la bandera
   0x100 solo se iluminan por cara a menos de 3 casillas (0xc00): **es un
   cambio de iluminación, no de geometría; no hay LOD de polígonos**.
8. **Sombra de Lara**: un solo polígono plano (como el octágono del port).

## Qué tiene el port que el original no

Todo el menú de modos: relieve, cel shading, sombras suaves/volúmenes, mapa
de entorno, filtrado, oclusión por rayos, 60 fps. Con modo Original apagado
queda la **misma cantidad de trabajo geométrico** que el PSX (polígonos medios
700-1100, peor caso ~2060 = el tope del PSX): el original **no dibuja menos**.
La diferencia es el coste por polígono: la GTE transforma 3 vértices en 23
ciclos a 33 MHz (~0,23 µs/vértice); el port gasta ~1 µs por vértice en
`xform` (6,9 ms para ~6200 vértices en Flycast, que no modela cachés).

## Candidatos (por orden de valor esperado; ninguno medido)

1. **Medir en consola, no en Flycast.** Flycast da ~28 fps donde la consola
   da 15-20; sin cifras de la SH-4 real, cualquier micro-optimización es a
   ciegas. Propuesta: un contador en pantalla (con `bfont_small`, ya
   existente) con `logic/stage/pass/submit/pvr` y polígonos/vértices, en la
   build `release`, para leerlo en GDEMU sin puerto serie. Es lo primero.
2. **Presupuesto de polígonos y vértices por fotograma** (como el original,
   pero ordenado): al pasar de ~N polígonos, soltar primero lo lejano o de
   poca pantalla (sprites de efectos, estáticos y ítems pequeños), no lo que
   llegue tarde. El peor caso del port (2063 polígonos, 12 278 vértices) es
   justo donde caen los 15 fps. **Antártida (la vista al cargar) es la
   escena de prueba que pidió el usuario.**
3. **Niebla por malla en objetos** (punto 7): un factor por instancia en vez
   de por vértice. Ahorro pequeño (`vobj` ≈ 2,6 ms de 6,2 en total).
4. **Cara trasera siempre descartada en lo lejano** (punto 6): en caras
   lejanas de una cara, descartar antes de emitir. Comprobar si el batcher ya
   lo hace (`eback` ≈ 25 de 2434 caras: parece que casi no actúa).
5. **Trozo contra la ventana del portal antes de transformar** (punto 5): el
   port ya marca los vértices fuera de la ventana (`M_CLIP_LEFT…`) pero los
   transforma. Poco que ganar: `eclip` ≈ 107 de 2434 caras.
6. **Datos de sala empaquetados** (punto 5): el port lee 28 bytes por esquina
   y escribe 32 por vértice transformado; el PSX 4 y 8. Solo se notará en
   consola (ancho de banda y caché de 16 KB); ver `dc_xform_pref` y
   `dc_sq_vertices` en TRASPASO.md §3, sin probar en consola.
7. **512×240 en vez de 640×480** (punto 2): el modo `low_res` es 320×240;
   un 512×240 se parece más al PSX y reduce el relleno de la PVR. Solo ayuda
   si la GPU es el límite en consola (con Antártida y sus listas
   translúcidas puede serlo).

## Descartado por el original

- LOD de geometría: no existe en el PSX.
- Menos polígonos que el PC: el tope del PSX (~2000) es del mismo orden que lo
  que dibuja el port.

## Partículas y clima en el PSX (verificado, 05/11/2026)

- **No hay lluvia ni nieve cayendo.** La rutina de "exterior" (0x1f060, rejilla
  de 27×27 celdas de 4096 en 0x9c6ec, la que usa el `DoSnow`/`DoRain` del PC)
  no tiene ningún `jal` ni puntero que la llame; el cargador de nivel (0x68ad4)
  construye la tabla, pero nadie la consulta para el clima. Para Antártida y
  la mina (niveles 16 y 17) solo hay 0x34e64, llamada por tick desde 0x1ac28:
  suelta **vaho** en la cabeza de Lara (una partícula cada pocos ticks,
  0x33fb8). El copo del port es una función del PC, no del PSX.
- **Reserva fija de 192 partículas** (0xc0 de 0x38 bytes, en 0xa2f38). Hueco
  libre por búsqueda circular (0x2e814; si no hay, reutiliza la más vieja).
  Los 30 tipos de chispa/humo/sangre/salpicadura (0x2f160…0x341f4) la usan.
  Actualización por tick: 0x2e97c; dibujo: 0x73560.
- **Dibujo de una partícula (0x73560), por partícula, sin matriz ni malla**:
  1. descartar si |dx|, |dy| o |dz| respecto a la cámara ≥ 0x5000;
  2. una sola proyección (RTPS) del centro → (sx, sy) y z;
  3. descartar si z < 0x100 o z ≥ 0x5000;
  4. tamaño: ancho y alto en bytes del hueco; si lleva la bandera de escala,
     `w' = w·persp / z` (con `<<` de un exponente propio), nunca mayor que el
     ancho base y con un mínimo de 1 (2 o 4 en el caso aditivo); sin la bandera
     es fijo;
  5. descartar si el rectángulo no toca la pantalla (0..0x200 × 0..0xf0);
  6. color = el del hueco; si z > 0x3000 se escala por `(0x5000 − z) >> 13`
     (el mismo fundido a negro que la niebla);
  7. se escribe **una primitiva** (rectángulo plano sin textura, o un quad con
     textura de 4 vértices con la coordenada del hueco) y se enlaza en la tabla
     de orden en z >> 3. Sin giro ni rotación salvo la bandera 0x10 (ángulo en
     el hueco, 4 vértices rotados con una tabla de senos).
- **Vaho de Lara y niveles fríos**: 0x34e64 elige la posición de la cabeza de
  Lara y llama a 0x33fb8, que rellena un hueco con vida 0x25-0x28, velocidad
  aleatoria pequeña y color gris 0x20.
