# Port de TRX a Dreamcast: estado de avance

Fecha: 18 de octubre de 2026 · Rama `dreamcast`, 13 commits sobre TRX ·
Toolchain: DreamSDK / KallistiOS 2.3.0

## Objetivo

Portar TRX (LostArtefacts) a la Sega Dreamcast: primero Tomb Raider II y
después Tomb Raider III. Debe funcionar en Flycast y en consola real con
GDEMU. El primer hito era un prototipo jugable. **Está cumplido**: el primer
nivel de TR2 (La Muralla China) se juega en Flycast.

## Resumen

| Área | Estado |
|---|---|
| Compilación y arranque en KallistiOS | ✅ Hecho |
| Carga de niveles desde el CD, con pantalla de carga (porcentaje y fichero) | ✅ Hecho |
| Render PowerVR: mundo, objetos, Lara, profundidad correcta, cielo detrás | ✅ Hecho |
| Filtrado bilineal (opcional, en ajustes) sin costuras | ✅ Hecho |
| Iluminación de TR2 y niebla por vértice | ✅ Hecho |
| Distancia de dibujo y niebla originales (12–20 casillas) | 🧪 Por probar |
| Sombra de Lara por volúmenes modificadores, según la luz cercana | ✅ Prueba de concepto |
| 30 FPS estables en el primer nivel (Flycast) | ✅ Hecho |
| Mando de Dreamcast (Start = inventario) | ✅ Hecho |
| Inventario: anillo 3D con fondo capturado del juego | ✅ Hecho |
| Menú de ajustes y de gráficos | ✅ Hecho |
| Armas en manos y fundas sin el sistema de trajes | ✅ Hecho |
| Morir y reiniciar el nivel | ❌ Se cuelga (falta memoria) |
| Gatillo L como modificador | 📝 Propuesto |
| Audio, FMV, guardado en VMU, efectos, textura del cielo | ⏳ Pendiente |
| Trenza de Lara y armas largas a la espalda | ⏳ Pendiente |
| Prueba en consola real (GDEMU) | ⏳ Pendiente |
| Tomb Raider III | ⏳ Pendiente |

## Lo que funciona

### Render (PowerVR)

TRX dibuja con OpenGL 3. En la Dreamcast se reemplazó por un renderer propio
sobre la API PVR de KallistiOS (`src/platform/dreamcast/render/`).

- **Vértices**: se transforman con la instrucción `ftrv` del SH-4 y se envían
  por DMA a cuatro listas: opaca, translúcida, punch-through y volúmenes
  modificadores. Los quads van como tiras de cuatro vértices, lo que ahorra
  un tercio de buffer.
- **Profundidad**: el PowerVR usa la misma Z para el test de profundidad y
  para la perspectiva de las texturas. Por eso el mundo usa `1/w` puro, el
  cielo y el fondo se escalan para quedar detrás de todo, y las capas 2D se
  apilan por encima.
- **Filtrado bilineal**: está activado por defecto y se puede desactivar.
  Las coordenadas de textura se desplazan medio texel para que no se vean
  las texturas vecinas del atlas.
- **Niebla**: se calcula por vértice con el color *offset* del PowerVR. Con
  la distancia original de TR2, lo que la niebla tapa del todo no se dibuja,
  ni por objeto ni por cara.
- **Sombras**: Lara proyecta un volumen modificador en modo *cheap shadow*.
  La dirección la marca la luz más cercana en un radio de 3 casillas
  (luces de la sala, fogonazo del arma, bengala) o, si no hay ninguna, cae
  vertical.
- **Fondo de menús**: la escena se renderiza a una textura (*render to
  texture*), porque en Flycast no se puede copiar el framebuffer.

### Rendimiento

El primer nivel corre a 30 FPS estables en Flycast. Para conseguirlo:

- `fsqrt` por hardware (`-fno-math-errno`).
- Matrices en `float` en lugar de `int64`.
- Luces de cada instancia precalculadas.
- Menos impresión por el puerto serie.

### Memoria (16 MB en total)

Es el mayor límite. El heap llega a unos 12,5 MB y el nivel ocupa ~11,1 MB.

- Las mallas de objetos se construyen bajo demanda y se expulsan si no se
  usan durante 300 frames.
- Si una reserva falla, un manejador de memoria baja expulsa mallas y fuerza
  la recolección de basura de Lua.
- Lua tiene su propio asignador y una recolección más agresiva. En DC se
  saltan los scripts de `commands/`.
- `lara_outfits.bin` (5,3 MB) no va en el disco. Lara usa sus mallas
  clásicas, con rutas propias para armas y caras.

### Controles

| Botón | Acción |
|---|---|
| Cruceta | Moverse |
| A | Acción |
| X | Saltar |
| B | Rodar |
| Y | Sacar armas |
| L | Mirar |
| R | Caminar |
| Start | Inventario |

Un hilo lee el mando cada 8 ms. Así no se pierden pulsaciones cortas y se
pueden pulsar varios botones a la vez.

### Herramientas (`tools/dreamcast/`)

| Herramienta | Uso |
|---|---|
| `build.sh` | Compila |
| `make_disc.sh` | Genera el CDI con los datos del TR2 original del jugador, que nunca entran en el repositorio |
| `run_flycast.ps1` | Lanza Flycast capturando el log serie |
| `flycast_shot.ps1`, `flycast_keys.ps1` | Capturas de pantalla y teclas automatizadas |
| Perfilador por muestreo | `dc_profile`, se lee con `prof_report.py` |
| Watchdog | Informa de memoria y estado cada 10 s |
| Rastreador de reservas | Builds de depuración: lista los bloques grandes vivos con su traza |

## Problemas conocidos

1. **Cuelgue al morir y reiniciar el nivel.** En la segunda carga se reservan
   unos 1,75 MB más que en la primera (11,7 MB frente a 9,9 MB en el mismo
   punto). Por eso falla la reserva de 1,6 MB de los fotogramas de animación.
   Para encontrar qué no se libera entre cargas, el rastreador de reservas
   vuelca los bloques vivos al empezar cada carga.
2. **Partes de Lara que desaparecen con la cámara muy lejos.** En las vistas
   abiertas el buffer de polígonos opacos (160 KB) se llenaba y se perdían
   ~900 polígonos por frame. Lara se dibuja al final, así que era lo primero
   que se perdía. **Corrección en prueba**: la niebla y la distancia de
   dibujo vuelven a las del original (12–20 casillas) y lo que queda detrás
   de la niebla ya no se envía.
3. **Rendimiento en consola real sin medir.** Se estima entre un 40 y un 50 %
   de probabilidad de llegar a 30 FPS tal como está, y entre un 75 y un 85 %
   con más optimizaciones. Conviene probarlo pronto en GDEMU.

## Próximos pasos

1. Arreglar el cuelgue al reiniciar el nivel.
2. Confirmar la corrección de la distancia de dibujo.
3. Acordar la distribución del mando con L como modificador. La bengala
   volvería a estar disponible, por ejemplo con L+Y.
4. Probar en hardware real con GDEMU y medir el rendimiento.
5. Audio (AICA), textura del cielo y efectos (FX).
6. Guardado en VMU y FMV.
7. Versión reducida de `lara_outfits` para la trenza y las armas a la
   espalda.
8. Resto de niveles de TR2 y, después, TR3.
