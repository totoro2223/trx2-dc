# Plan: pantalla panorámica anamórfica y cargas más rápidas

Plan en curso (09/11/2026). Dos trabajos independientes.

## A. Pantalla panorámica anamórfica (16:9)

**Qué es.** La consola sigue sacando 640×480, pero el juego dibuja la escena como si la pantalla fuera 16:9. El usuario pone la tele en 16:9 y la imagen se estira a lo ancho. Cuesta los mismos píxeles que hoy.

**Qué hay hoy** (leído, no probado):
- `Output_GetPerspProjectionMatrix` (`src/trx/game/output/state.c:286`) saca el aspecto de `Viewport_GetWidth/Height(VIEWPORT_GAME)`, que en DC es 640×480 = 4:3.
- `M_GetFocalLengths` (`state.c:69`) reparte el FOV según `FOV_MODE`: horizontal, vertical, PC, PS1. Con el modo PC el aspecto 4:3 ya está fijado a un factor `(4/3)/aspect`.
- El batcher toma la proyección de `matrices->mat_proj[0][0] * half_w` (`batcher.c` ~7585), así que basta con cambiar `f_x`.

**Pasos**
1. Opción de ajustes `Pantalla: 4:3 / 16:9 anamórfico`, guardada en la VMU con el resto de ajustes. Por defecto 4:3.
2. En 16:9, usar aspecto 16/9 en `M_GetFocalLengths` y `Output_GetVerticalHalfFOV` (el framebuffer sigue siendo 640×480). La imagen resultante sale más ancha de lo que dice el FOV y la tele la estira a su sitio.
3. Todo lo 2D (menús, imágenes de carga, fondo del título, HUD, pasaporte, anillo, texto): escalar en x por 3/4 al dibujar para que no salga estirado en la tele. Está en `ui.c` y en el camino ortográfico del batcher. Las pantallas completas (carga, FMV) se pueden dejar sin escalar y estirarse, o escalar y dejar bandas negras; decidir al ver cuál se ve mejor.
4. Visibilidad: la ventana de portales y el recorte lateral usan el ancho del viewport. Con más campo visible a los lados entra más geometría. Comprobar que no se corta nada en los bordes, y que la niebla y la distancia de dibujo no cambian (regla fija).
5. Rendimiento: ver más ancho significa más polígonos por fotograma. Medir con el build de sondas en TR1, TR2 y TR3 en 4:3 y 16:9, y apuntar la diferencia en PROBADO.md.
6. Probar en Flycast con su opción de pantalla ancha (para ver la deformación correcta) y luego en la consola con una tele 16:9.

**Riesgos**
- Las partículas, el brillo y las sombras que proyectan por pantalla (`proj_x`) podrían salir con el aspecto viejo.
- El cálculo de visibilidad hecho en asm (`room_sh4.S`, `xform_sh4.S`) puede tener el ancho de pantalla fijo a 4:3.
- Guardar el ajuste en la VMU toca el formato de configuración.

## B. Cargas más rápidas, sobre todo TR3

**Idea.** El menú principal y el título no deberían cargar lo que un nivel de juego. Hoy el título es un nivel completo (`GFL_TITLE`, `game_flow/reader.c`) y pasa por la misma tubería de carga (`level/pipeline.c`).

**Lo que ya se hizo** (30/10/2026, medido solo en Flycast): sonidos ADPCM con índice, orden de archivos en el disco, búsqueda del montón por bisección. Ver la memoria `dc-load-time-optimizations`. La consola real sigue sin medirse.

**Pasos**
1. **Medir antes de tocar.** Registro serie con el tiempo de cada fase en TR3 (menú y un nivel normal): lectura del `.TR2`, texturas, sonidos, inyecciones (`Inject_AllInjections`, unos 3,6 s), caché de transparencia de objetos (tarea 66), mapa de solapes de salas, creación de enemigos y rutas. Sin esto no se sabe qué quitar. Apuntar el resultado.
2. **Carga mínima para el título y el menú.** Saltar en `GFL_TITLE` lo que no se usa: sonidos de juego, mallas y animaciones de enemigos, inyecciones que no afectan al título, cajas y rutas (LOT), caché de transparencia de objetos. Cargar solo salas, texturas y lo que la cámara del título muestra.
3. **Carga mínima en niveles.** Cargar solo las muestras que el nivel usa (ya hecho con el índice), solo las texturas realmente referenciadas, y los objetos que el nivel coloca.
4. **Agrupar archivos pequeños.** Las ~21 inyecciones por nivel se leen una a una: empaquetarlas en un archivo (sin medir si ayuda en la consola; GDEMU puede no notarlo).
5. **Guardar el resultado de las inyecciones.** Es el trozo más caro; exige copiar el estado del nivel, no hecho (tarea 77).
6. **Probar en la consola real** cada paso, no solo en Flycast, porque el coste está en el disco.

**Riesgos**
- Cargar menos puede dejar un objeto o un sonido sin cargar en un nivel concreto: la prueba son los tres juegos enteros (hay una batería de memoria ya hecha, ver `REPORTE_MEMORIA.md`).
- El título de TR3 puede depender de objetos que se ven en pantalla; hay que mirar cuáles.
- El tiempo de la consola solo lo puede dar el usuario.

## Estado (09/11/2026)

B1 hecho (registro `[loadtime]`), B2 hecho (título de TR3 mínimo), y de B3-B5 la mitad: las páginas de las inyecciones como hash. Lo que queda de B está en TAREAS.md (86). A (panorámico) en curso, sin commit ni prueba: viewport de 853×480 comprimido a 640 en x (`DC_PVR_WIDE_WIDTH`/`DC_PVR_WIDE_X`, `pvr.h`), fila "Screen" que se aplica en la carga siguiente (`g_DC_WidePending`, `nolua_settings.c`, `loading.c`) y gancho `TRX_DC_WIDE`; otro camino que el de A2/A3 (que cambiaban `M_GetFocalLengths`). Supuesto a partir del diff, sin compilar.

## Orden propuesto

1. B1 (medir): no cambia nada y dice dónde está el tiempo.
2. B2 (título mínimo): da la mejora más visible.
3. A completo (panorámico).
4. B3–B5 según lo que enseñe la medida.

## Decisiones del usuario (09/11/2026)

- Se puede probar en la consola real, con tele 16:9.
- Solo el juego (in-game) va en panorámico.
- Las pantallas de carga y los FMV mantienen su aspecto 4:3 y se muestran en
  4:3 dentro del 16:9 (bandas negras a los lados), sin estirar. Cambia el paso
  A3: carga y FMV no se escalan en x por 3/4 ni se estiran, se centran a 4:3.
- Menús (título, ajustes, pasaporte, anillo): 4:3 centrado, como carga y FMV.
- HUD: escalado en x por 3/4 a todo el ancho, pegado a los bordes, para que no
  salga estirado.

## Preguntas abiertas

Ninguna.
