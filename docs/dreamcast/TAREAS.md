# Tareas pendientes

Solo lo que falta. Lo hecho y lo descartado está en [PROBADO.md](PROBADO.md)
(resumen) y [REGISTRO.md](REGISTRO.md) (detalle): **míralos antes de empezar**
para no repetir una idea que ya falló. Entorno, ciclo de compilación y reglas
con Flycast: [TRASPASO.md](TRASPASO.md).

Lo hecho sin medir o sin ver está en [PRUEBAS_PENDIENTES.md](PRUEBAS_PENDIENTES.md).

Actualizado el 28/10/2026 (tras cerrar P1/P2/P5 de memoria).

**Memoria en sesiones largas** (niveles seguidos, cargar/guardar, cambio de
juego, reinicio de nivel): plan completo de pruebas P1–P13 y correcciones
M1–M13 en [PLAN_MEMORIA_SESIONES_LARGAS.md](PLAN_MEMORIA_SESIONES_LARGAS.md).
Cubre las filas 64, 65, 66, 67 y 68 de la sección J.

Al terminar una tarea: anota la fila en PROBADO.md (OK o FALLIDO, con cifra y
build), el detalle en REGISTRO.md, y **bórrala de aquí**. Retira las sondas
`TEMP` (`dc_probe.h` y los `DC_PROBE(...)`) antes de dar un commit por bueno.

Reglas comunes: cambios en código compartido bajo `#if defined(__DREAMCAST__)`;
nada de `build/`, CDIs ni datos del juego en git; no tocar un Flycast abierto
por el usuario; decir siempre "DC build N"; lo que no gane nada medible se
deshace y se anota como FALLIDO.

---

## A. Comprobar lo ya compilado (prioridad alta)

1. **Correr las pasadas de niveles con la piscina de 384 K** (`M_POOL_SIZE`,
   `dc_level_heap.c`): compilada y sin probar. Los discos `*-run2.cdi` (DC
   builds 323–325) son de **antes** de `e139434fb`/`f3b6f427d`: compilar y
   hacerlos de nuevo (`TRX_DC_LEVELS='<último> 300' TRX_DC_SAVE=90`, ver
   REGISTRO "Para continuar"). Mirar que el hueco mayor (`game heap: ...
   largest free`) ya no baje ~90 K por carga (J 69), si TR2 pasa Barkhang
   (J 64) y si TR3 pasa Lud's Gate (J 65).
2. **Ver a ojo los cuatro niveles de sombra** en TR2 y TR3: Soft (high) con
   luces cercanas (deformación de UV en suelos grandes), Hard con
   magnificación, y medir su coste frente a Soft (low).
3. **Ajustar Adaptive 60 FPS** en un nivel pesado (Londres, Bartoli): los
   umbrales 33 ms / 20 ms y los 240 frames de espera están sin probar.
4. **Ver en pantalla** lo de la lista "sin ver" de PROBADO.md (UI reutilizada,
   sombra bajo el agua, sangre y bengala de TR3).
6. **Ajustar a ojo el relieve con brillo** (25/10/2026): `M_BUMP_DEPTH` 0,9 y
   `M_BUMP_GLOW` 0,5, la pasada `DC_BLEND_BUMP_GLOW`, las luces de paso (8×
   de peso, 3× de alcance) y el apagado bajo 20 fps (`M_BUMP_MIN_FPS`).
   Medir su coste en Londres frente al relieve anterior.

## B. Consola real

5. **Probar en GDEMU y medir.** Flycast no modela las cachés del SH-4 (16 KB de
   datos, 8 KB de instrucciones). Hasta entonces, nada que dependa de la caché
   se puede dar por bueno ni por malo: `M_EmitChunk`, `movca.l`, descarte de
   espaldas por plano, datos calientes/fríos del vértice de sala. Con
   `dc_profile` y `prof_report.py` solo hay una cota optimista.
71. **Vértices por store queues hacia la RAM** (visto en TR4 de Dreamcast,
    REGISTRO 27/10/2026): escribir cada vértice de 32 bytes en el búfer de la
    lista a través de las store queues (QACR0/1 apuntando a la RAM del búfer,
    `ocbi` de las líneas de destino y un `pref` por vértice) en vez de con
    stores que pasan por la caché. Solo se mide en consola. **Hecho como
    opciones** (28/10/2026, DC build 823 en Flycast: se ve bien):
    `dc_sq_vertices` y `dc_xform_pref` (`pref` en los bucles de
    transformación); cómo encenderlas en TRASPASO. Falta medir en GDEMU con y
    sin ellas: los fps en el mismo sitio (en `release` no hay `[frame]`; para
    los tiempos, `minsize` con las mismas opciones). No hecho: encadenar el
    `ftrv` de un vértice con el siguiente (la transformación entera es
    ~0,5 ms por frame).

## C. Optimización del render (lo que de verdad queda)

Reparto de Londres (graphics, ms por frame): pass ~12,7 (estáticos y sprites
3,2, emisión 3,0, objetos 1,6, salas 1,5), stage ~7, Lua ~2,6, lógica ~4,2.

7. **`UI_RESERVE_ALL`**: investigado el 23/10/2026 (DC build 439, Londres). Lo que
   toma una raíz ya se guarda entre escenas y ninguna raíz es volátil (ningún
   `Tween`). La región 2 se recalcula en rachas (142 de 300 escenas, y luego
   casi ninguna), por un widget que cambia cada tick; el resto, una vez cada
   ~30 escenas o menos. No hay un fallo que arreglar; el tope de ganancia es
   ~0,15 ms. Solo merece la pena si se averigua qué widget de la región 2
   cambia cada tick.
8. **`TICK` en Lua** (~0,9 ms): sin medir por listener. Medir antes de agrupar
   las lecturas de roles de `input.lua` (era una suposición).
9. **30 reservas por frame** de `Memory_Alloc` (`Catalog_CreateKey`,
   `Memory_DupStr`, `String_Format`).
10. **`Sparks_Control`** (0,2–0,9 ms): hace `Random_GetControl` y luces
    dinámicas; no se puede saltar tal cual.
11. **Emisión** (3,0 ms): es volumen de instrucciones repartido; bajar más pide
    menos caras miradas o vértices ya listos para el PowerVR. Sin pista barata.
12. **Mezcla de matrices de Lara** (`lblend` 0,33 ms): solo vale ~0,16 ms y
    exige saber qué relación guardan las dos pilas (vista y mundo) en DC.
13. **Página de relieve hecha en juego**: con `textures.bump` solo queda para
    las páginas que el disco no tiene (las de las inyecciones y las que
    editan); su primer bucle (`DC_Bump_HalfTexel`/`M_PageBlock`) sigue siendo
    por texel. Antes, comprobar en Flycast el relieve del disco (PROBADO, fila
    del 27/10/2026).

## D. Memoria

15. **Datos crudos de sala** (~1,7 MB): `shadows.c` los lee en juego; hay que
    separar lo que necesita antes de poder liberarlos tras `M_ShareVertices`.
16. Tablas de cadenas, animaciones.

## E. Funcionalidad

17. **TR3**: chispas sin interpolación ni giro (`SPARK_F_ROTATE`) y cuadradas;
    salas de color sin el mínimo de sombra ni el parpadeo. (Los guardados de
    TR3 en la VMU salen como `save_tr3_*.dat` → `TRX_TR3_*`: comprobado.)
18. **Experimental 60**: franja negra arriba a 320×240; comprobar la vuelta a
    640×480. (El define `TRX_DC_MMU_NULL_TRAP` ya no está en `meson.build`.)
19. **Inventario**: 9 → 22 fps, no se ha vuelto a medir tras la arena de Lua.
20. **Audio**: reverb y filtro bajo el agua (programa propio del DSP del AICA).
72. **FMV antes de cargar el nivel** (aparcada el 27/10/2026): hoy
    `GF_InterpretSequence` (`sequencer.c`) carga el nivel antes de su
    secuencia, así que se ve barra de carga → FMV → imagen de carga → juego;
    los discos originales hacen FMV → carga → juego. En DC, reproducir las
    `play_fmv` que abren la secuencia antes de `Level_Initialise` (como los
    eventos de `M_IsPreLoadEvent`), saltándolas después. Riesgo: el nivel
    anterior sigue en memoria mientras el reproductor pide ~480 KB; probarlo
    en el nivel más pesado de cada juego. Las cinemáticas de TR1–3 son niveles
    completos (TR4 las reproduce sobre el nivel cargado desde `cutseq.pak`,
    REGISTRO 27/10/2026), así que sus dos cargas no se quitan así.
21. **Deseos del usuario**: gatillo L como modificador (bengala con L+Y), textura
    del cielo, efectos, Sonido/Controles/Juego en el menú. (Hechos: FMV, armas
    largas a la espalda, todos los niveles de TR1, TR2 y TR3.) Aparcado: pantalla Rendering mínima de DC, mipmaps del mundo,
    subir `vertex_buf_size` de PVR (512 K).

## F. Bug aparcado

22. **Heap corrupto en la demo del Ganges (TR3)**. El usuario cree que es de
    esa demo y no vale la pena seguir. Solo reabrir si aparece en otro nivel.
    Reproducir: `TRX_DC_GAME=tr3 TRX_DC_DEMO=<n>` en `make_disc.sh`.

## G. Stubs que afectan a TR1–TR3

El motor compartido ya decide qué dibujar; lo que falta en Dreamcast es el
destino de dibujo: funciones vacías `DC_STUB()` en
`src/platform/dreamcast/render/stubs.c` (60) y `av_stub.c` (vídeo y
decodificador de audio). Para cada una: implementarla en `render/`, medir y
anotar en PROBADO.md.

**Cómo se hizo la tabla** (23/10/2026): se buscaron los llamadores en
`src/trx/game` y se asignó el juego por la guarda `g_TRVersion` o por el objeto
al que pertenece el efecto. Lo marcado "(probable)" sale del conocimiento del
juego y **no está comprobado en el código ni en pantalla**; confirmarlo al
tomar cada tarea. El código compartido también cubre TR4, que queda fuera del
port. Si TR1 entra en el port (decisión pendiente, ver H), lo único que
cambia es la columna TR1; casi todos los efectos pesados no existen en TR1.

| # | Stub (llamadores) | TR1 | TR2 | TR3 | Prioridad |
|---|---|---|---|---|---|
| 31 | **Depuración** (`Misc_Stage{Cuboid,Sphere}`, `RoomsDebug_*`): cajas de choque, ascensor, puertas de disparo, `lua/capi/scene.c` | sí | sí | sí | Baja: no son efectos del juego |

**Cielo** (`OutputSource_Sky_StageLayer` y `StageFogGradient`): no aplica. Solo los llama el código de TR4 (`g_TRVersion == 4` en `output/sky.c`); el cielo de TR2 y TR3 es una malla normal y ya se dibuja. Los stubs se quedan vacíos.

Quedan fuera de la tabla los 12 stubs `TRX_GL_*` (contexto OpenGL, temporizador
de GPU, capturas): no se dibujan en Dreamcast ni hacen falta.

Sin cubrir en el código compartido (no hay stub pero tampoco se ha visto en
pantalla): lens flares y bulbos de niebla de TR3, y el efecto de superficie del
agua (`Output_Water_*`). Comprobar en un nivel de TR3 con sol y con agua.

## H. Expansiones

Hechas (ver PRUEBAS_PENDIENTES.md): Unfinished Business, Golden Mask y The
Lost Artifact con los datos del remaster (`TRX_DC_MOD=tr1-ub|tr2-gm|tr3-la`).
Desde `f3b6f427d` van en todas las imágenes, en la página 4 del selector
previo al motor y en "Play Expansion" del pasaporte; "Return to Egypt" arranca
`egypt.phd` (Flycast). Falta jugarlas, y lanzar la de TR2 y la de TR3 desde el
selector.

## I. Efectos de TR3

Hechos (compilados): ver T47 a T49 y T52 en
[PRUEBAS_PENDIENTES.md](PRUEBAS_PENDIENTES.md). El humo de las pistolas y el
fuego no se dibujaban porque `Sparks_Draw` volvía antes en Dreamcast (quitado,
T50); falta verlos en consola.

## J. Abierto tras las pruebas del 24/10/2026

(Las filas 60 y 61, la falta de memoria de TR2 y TR3 al pasar de nivel en
nivel, se resolvieron: T59 y T60 en PRUEBAS_PENDIENTES.md.)

| # | Qué | Detalle |
|---|---|---|
| 59 | **Sombra suave sobre la superficie del agua (TR2)** | No se pudo reproducir: el código ya salta las salas bajo el agua mientras Lara no está sumergida (`is_room` en `batcher.c`). Hace falta una captura con el sitio exacto (nivel y postura de Lara). |
| 62 | **Armas largas en la espalda: lanzagranadas, arpón (TR2), MP5 y cohete (TR3), escopeta de TR1** | El modelo del inventario puede faltar en el nivel, o salir mal girado. Probar con `TRX_DC_GIVE`. |
| 63 | **Disco de tres juegos en consola real** | El cambio de juego ya no reinicia (T70) y la elección queda en la VMU; falta verlo en consola. Salir al selector de niveles tras el juego sigue reiniciando el disco (`arch_reboot`). Tamaño 1,0 GB sin películas. |
| 64 | **Memoria en sesiones largas: batería cerrada** (P1, P2, P5 OK). El guardado de Barkhang y la carga de TR3 ya están arreglados y el detalle está en [REPORTE_MEMORIA.md](REPORTE_MEMORIA.md). | P2 TR3 completo 0–18 pasa con `[levels] done` (build 804, 66 guardados / 19 cargas, log `build/logs/2026-10-28/dc-mem-P2-tr3e-fresh.txt`). Al cargar el nivel 19 hay `Out of memory` de newlib y watchdog pero acaba pasando. Falta P3 (cargas repetidas), P4 (pico de Barkhang con guardados), P8 (reiniciar/cambiar de juego), P11 (sesión larga). Antes de dar el commit por bueno, quitar las sondas TEMP `[save]` (`savegame/file.c`) y `[arena]` (`dc_save_arena.c`). |
| 65 | **Flycast se cierra solo** en Lud's Gate (TR3, nivel 11) y en la pasada de niveles, sin excepción ni aserto en el registro serie (VRAM libre 2,4 MB). | El fallo era al aplicar la partida (árbol BSON de 713 K). Arreglado leyendo el árbol en trozos con la arena (builds 800–805). La pasada completa de 19 niveles pasa ahora; queda como observado en el pico del nivel 19 (hay reintentos de newlib) pero sin caída. |
| 66 | **Caché de transparencia de objetos** (`object_transparency_*.cache.dat`): el disco RAM no tiene directorios y nunca se escribe, así que cada carga vuelve a dibujar todas las mallas para hallarla (y hace crecer las matrices del batcher en el pico). | Generarla al hacer el disco (`make_disc.sh`) o guardarla en `/ram` a secas. |
| 67 | **MIGRACIÓN IMPORTANTE DE LÓGICA: memoria por arenas con época y zona estática acotada** (anotada el 24/10/2026; no empezada, para hacer en otra ocasión). Es la única vía sistemática para "esterilizar" la RAM al cargar un nivel, salir al menú o cambiar de juego sin reiniciar la consola: un limpiador general no existe porque el motor guarda punteros directos (no se puede compactar), y limpiar solo libera lo que alguien liberó. Pasos: **(1)** que todo el motor reserve desde arenas con "época" (permanente, partida y nivel; hoy solo el nivel lo hace) para soltar la arena entera al terminar una partida o cambiar de juego; **(2)** que todas las variables estáticas del motor queden en una zona de memoria acotada con el enlazador (secciones de datos y bss propias), para ponerla a cero o restaurarla entera al cambiar de juego (hoy cada módulo se limpia a mano: `m_IsExiting`, `PhaseExecutor_Reset`, los interruptores de las pruebas, `TRX_DIR`; se escapan los que no se conocen); **(3)** dejar fuera de esa zona lo de KallistiOS y newlib, porque sus drivers siguen vivos (sonido, vídeo, mandos) y tocar su estado los rompe. Esfuerzo de varios días y riesgo alto: un puntero estático que se escape queda colgando. **Lo que queda aparte y no resuelve esta migración:** el pico de carga de TR2 Barkhang y TR3 (tareas 64), las 287–299 K que cada juego deja retenidas, la fragmentación del montón de newlib por objetos pequeños (Lua, cadenas) y salir al selector de niveles, que todavía reinicia el disco. | Decidirlo con un caso real delante (qué falla, en qué juego y nivel). Medir antes con `live_census.py` qué retiene cada juego. |
| 68 | **Sin efectos de sonido tras cambiar de juego** (solo suenan los de fondo) al ir cambiando de juego con los títulos y entrar y salir de niveles o cargar partidas; ya no se cuelga. Observado por el usuario el 24/10/2026, sin investigar. Hipótesis (sin comprobar): al apagarse, `Audio_Shutdown` (`audio.c`) no libera los canales de efectos que `Audio_Init` reserva con `snd_sfx_chn_alloc`, y el segundo arranque en el mismo proceso se queda sin ellos. | Mirar `Audio_Init`/`Audio_Shutdown` y repetir el cambio de juego con el registro serie. |
| 69 | **Deriva del hueco mayor de la región por carga** (~90 K por carga en TR1, build 320: 6271 → 5448 K en 13 cargas, con solo 3 bloques retenidos): la piscina de 256 K se llenaba durante la carga, lo que sobraba se apilaba bajo el muro y el mapa de solapes de salas nuevo caía cada vez más abajo. Piscina a 384 K, sin probar (tarea A1). | Si sigue bajando: liberar el mapa de solapes en `Level_Unload` (hoy lo libera `Room_InitialiseRooms` de la carga siguiente, `rooms/common.c`) o reservarlo en la cabeza (fuera de la ventana del nivel). |
| 70 | **Las 287–299 K que cada juego deja retenidas** al cambiar de juego en el mismo proceso (T70) | Con `-Ddc_heap_census=true` y `live_census.py` tras un cambio de juego. |
