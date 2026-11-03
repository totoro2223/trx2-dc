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

**Refactor para bajar la RAM** (02/11/2026: revisión del fork OpenLara-dc,
reparto de la RAM, fases A–C con ganancias estimadas): ver
[PLAN_REFACTOR_RAM.md](PLAN_REFACTOR_RAM.md).

**Geometría que no se ve** (objetos tapados por paredes, trozos más finos en
salas grandes, tope del relieve): plan G1-G4 en
[PLAN_GEOMETRIA_OCULTA.md](PLAN_GEOMETRIA_OCULTA.md).

**VRAM libre** (bytecode de Lua a VRAM, mipmaps del mundo, mapa de sombra
suave de 128): plan V1–V3 en
[PLAN_VRAM_LUA_MIPMAPS_SOMBRA.md](PLAN_VRAM_LUA_MIPMAPS_SOMBRA.md).
V1 (Lua en VRAM) hecho y OK en Flycast. V3 (mapa de sombra a 128) revertido a 64: a 128 faltan caras de las salas (ver PROBADO). V2 (mipmaps) apagado por defecto: corrompe las páginas con texels transparentes en Flycast; `TRX_MIP=1` en `texvq` los activa para investigar. Borrar esta nota cuando se resuelvan.

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
14. **Matemática de enteros a FPU o ensamblador** (análisis 02/11/2026, sin
    medir; Flycast no modela el coste, así que la ganancia solo se ve en
    consola: ver la fila FALLIDO de `fsrra` en PROBADO). Por orden:
    a. (HECHO 02/11, sin medir en consola) `Math_Sqrt`/`Math_Sqrt64` (`core/math/func.c`): raíz por bits (16 y 32
       vueltas, la de 64 sobre enteros de 64 en 32 bits). Usada en
       distancias, colisión, cámara, IA y vehículos. Pasar a `fsqrt`/`fsrra`.
    b. (HECHO 02/11) `Math_Atan` (`core/math/trig.c`): `0x800 * y / x` es una división
       entera por llamada y la SH-4 no la tiene. Recíproco en float o `div1`.
    c. (HECHO en C: seno/coseno una vez; falta la rutina `.S` solo si el perfil lo pide) Pila de matrices (`game/matrix.c`): `M_RotX/Y/Z`, `M_TranslateRel`,
       `Matrix_Mul3x3_M`, `Matrix_Push`. Cada operación se repite en 2
       matrices (vista y mundo), 4 en las `_I`/`_ID`, y `Math_Sin/Cos` se
       llaman en cada una. Una rutina `.S` con seno/coseno una vez y
       `dmuls.l`/`mac.l` sobre todas. Parte se gana ya en C calculándolos una
       vez. Lara hace 73 llamadas en `lara/draw.c`, 40 en `hair.c`.
    d. `M_InterpolateFloat`: `sqrtf`, división, `acosf` y 3 `sinf` por
       mezcla. `fsrra` para 1/raíz; `fsca` o mezcla lineal siempre. Enlaza con
       la 12 (~0,16 ms).
    e. `Math_Sin`: la rama `m_UseCoarseTable` y `Math_Cos` que la llama otra
       vez; inline sin la rama en DC (es C).
    f. `M_Turn`/`XYZ_32_Rotate*` (`core/math/geom.c`): solo si el perfil lo
       señala (el pelo de Lara es el mayor usuario).
    Cada una con la comprobación de 0 diferencias de las rutinas `.S`
    anteriores. No pasar a ensamblador: relieve, `Sparks_Control`,
    `Memory_Alloc`, Lua (PROBADO).

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
73. **FMV en MPEG-1, a 5-7 fps de imagen** (30/10/2026, DC builds 861-865, aparcada): `TRX_DC_FMV_CODEC=mpeg1` en `make_disc.sh`; disco de prueba `TRX-tr2-mpeg1c.cdi` (mini de TR2, solo `ancient`). En Flycast la imagen ya sale bien (el ruido era por no mandar los 512 macroblocks al convertidor YUV) y va sincronizada con el audio, pero se ven solo 5-7 fps. Medido (build 863): 21 ms por imagen decodificada, 3 imágenes por vuelta del bucle, 3,8 ms de presentación; el bucle de `fmv.c` tarda ~200 ms por vuelta, de los que `Video_PumpEvents` explica ~67: faltan ~130 ms sin atribuir (`LUA_Tick`, `Input_Update`, `Shell_ProcessEvents`, audio). La build 865 añade a la línea `film:` el tiempo entre vueltas (`ms apart`) y el de `M_Fill`: no se llegó a leer. **Siguiente**: leer esa línea en `build/dc-serial.txt`; si el hueco está fuera del reproductor, decodificar de una imagen por vuelta (`n < 4` en `M_ShowMpeg`) o sacar el decodificado a un hilo con dos buffers; comparar con una película VQ en el mismo bucle (¿también va a 5-7 fps?). Después, probar en consola (GDEMU). Hay sondas TEMP en `video.c` (`last_pump_us`, `gap_us`, `fill_us`, `pumps`). Detalle en `REGISTRO.md` y `PLAN_FMV_MPEG1.md`.
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
| 66 | **Caché de transparencia de objetos** (02/11: ya en memoria para cargas repetidas, falta la primera carga) (`object_transparency_*.cache.dat`): el disco RAM no tiene directorios y nunca se escribe, así que cada carga vuelve a dibujar todas las mallas para hallarla (y hace crecer las matrices del batcher en el pico). | Generarla al hacer el disco (`make_disc.sh`) o guardarla en `/ram` a secas. |
| 67 | **MIGRACIÓN IMPORTANTE DE LÓGICA: memoria por arenas con época y zona estática acotada** (anotada el 24/10/2026; no empezada, para hacer en otra ocasión). Es la única vía sistemática para "esterilizar" la RAM al cargar un nivel, salir al menú o cambiar de juego sin reiniciar la consola: un limpiador general no existe porque el motor guarda punteros directos (no se puede compactar), y limpiar solo libera lo que alguien liberó. Pasos: **(1)** que todo el motor reserve desde arenas con "época" (permanente, partida y nivel; hoy solo el nivel lo hace) para soltar la arena entera al terminar una partida o cambiar de juego; **(2)** que todas las variables estáticas del motor queden en una zona de memoria acotada con el enlazador (secciones de datos y bss propias), para ponerla a cero o restaurarla entera al cambiar de juego (hoy cada módulo se limpia a mano: `m_IsExiting`, `PhaseExecutor_Reset`, los interruptores de las pruebas, `TRX_DIR`; se escapan los que no se conocen); **(3)** dejar fuera de esa zona lo de KallistiOS y newlib, porque sus drivers siguen vivos (sonido, vídeo, mandos) y tocar su estado los rompe. Esfuerzo de varios días y riesgo alto: un puntero estático que se escape queda colgando. **Lo que queda aparte y no resuelve esta migración:** el pico de carga de TR2 Barkhang y TR3 (tareas 64), las 287–299 K que cada juego deja retenidas, la fragmentación del montón de newlib por objetos pequeños (Lua, cadenas) y salir al selector de niveles, que todavía reinicia el disco. | Decidirlo con un caso real delante (qué falla, en qué juego y nivel). Medir antes con `live_census.py` qué retiene cada juego. |
| 68 | **Sin efectos de sonido tras cambiar de juego** (solo suenan los de fondo) al ir cambiando de juego con los títulos y entrar y salir de niveles o cargar partidas; ya no se cuelga. Observado por el usuario el 24/10/2026, sin investigar. Hipótesis (sin comprobar): al apagarse, `Audio_Shutdown` (`audio.c`) no libera los canales de efectos que `Audio_Init` reserva con `snd_sfx_chn_alloc`, y el segundo arranque en el mismo proceso se queda sin ellos. | Mirar `Audio_Init`/`Audio_Shutdown` y repetir el cambio de juego con el registro serie. |
| 69 | **Deriva del hueco mayor de la región por carga** (~90 K por carga en TR1, build 320: 6271 → 5448 K en 13 cargas, con solo 3 bloques retenidos): la piscina de 256 K se llenaba durante la carga, lo que sobraba se apilaba bajo el muro y el mapa de solapes de salas nuevo caía cada vez más abajo. Piscina a 384 K, sin probar (tarea A1). El mapa ya se libera en `Level_Unload` (C4, 02/11/2026, sin probar). | Si sigue bajando: liberar el mapa de solapes en `Level_Unload` (hoy lo libera `Room_InitialiseRooms` de la carga siguiente, `rooms/common.c`) o reservarlo en la cabeza (fuera de la ventana del nivel). |
| 70 | **Las 287–299 K que cada juego deja retenidas** al cambiar de juego en el mismo proceso (T70) | Con `-Ddc_heap_census=true` y `live_census.py` tras un cambio de juego. |
| 71 | **Sombra suave más oscura** (pedido el 29/10/2026, sin hacer): `M_SOFT_STRENGTH_MAX` 0,9 → **0,95**; sin luz cerca (`M_SOFT_NO_LIGHT_WEIGHT`, hoy 0,3 = fuerza ≈0,69) → fuerza **≈0,75**; `M_SOFT_SELF_STRENGTH` (Lara sobre sí misma) 0,7 → **0,75**. | Constantes en `batcher.c`. El peso sin luz se despeja de `MIN + (MAX − MIN) * peso`: con MIN 0,6 y MAX 0,95, peso ≈0,43 da 0,75. Mirar cómo se ve junto al cel shading. |
| 72b | **Primera prueba en consola real (30/10/2026, minis release con DMA de TR1, TR2 y TR3, builds 891-896)**: en TR1 y TR3 los texels transparentes salen como bloques negros alrededor de helechos y hojas, y con un efecto verde/dorado encima (TR3, junto a Lara y en hojas colgantes); en TR2 el fondo del agua sale negro, la superficie vista desde abajo también, y con una bengala sobre la textura negra se ve el relieve (bump) aplicado a ella. Sin ver en Flycast. | **Modo Original en consola (TR1): los bloques negros persisten, no es el relieve.** Hipótesis a probar (builds 897-902, sin ver): `cxt.gen.alpha` estaba apagado en la lista punch-through (`pvr.c`, `M_SetState`); ahora está encendido para todo menos el opaco, porque la consola solo lee el alpha de la textura para el recorte con ese bit (Flycast corta de las dos maneras). Si sigue igual: probar el fondo del PVR en magenta (huecos o caras negras), y el agua de TR2 aparte (fondo y superficie negros) |
| 72 | **Transparencias en negro** (carámbanos de TR1, hojas de TR2 en consola real, humo/sombra/splashes de TR3 a veces en Flycast): los texels transparentes salen como bloques negros opacos. Sin resolver; no es de los mipmaps ni del alpha de la cabecera PUNCH. | Mirar con una traza qué mezcla y página lleva cada sprite que sale negro, y cómo se marca el alpha de esos texels. En consola real, probar TRX-tr2 con el fondo del PVR en magenta para ver si son huecos o caras negras. |
| 73 | **Mapa de sombra a 128** (+ caras que faltan en las salas de TR1): hipótesis sin comprobar, el render a 128 es 4× más lento y el PVR aún lo tiene en marcha cuando empieza la escena. | Probar 96 de lado o una espera extra (`pvr_wait_ready`) tras el mapa. |
| 74 | **Mipmaps VQ**: corrompen en Flycast las páginas con texels transparentes (Lara, fuente, pasaporte), aun con el sesgo en 0,25. Los datos (TVQ2) decodifican bien. | Comparar con la disposición real del PVR/Flycast; probar una página con alpha sola. `TRX_MIP=1` activa la generación. |
| 75 | **Plan de RAM pendiente** ([PLAN_REFACTOR_RAM.md](PLAN_REFACTOR_RAM.md), 02/11/2026): **probar** lo aplicado sin pruebas (A2, A3, C1/C2, D2 reducido, D7 parcial, C4; ver REGISTRO) y hacer lo que falta: **B** (niveles horneados `.DCL`), **D1** (una copia de cada sala), **D2 completo** (sectores decodificados al consultar), **D6** (un ejecutable por juego), **D7** (tabla de solo lectura para el hash del catálogo, tras el censo A5) y **C3** (arena de frame). | D1 exige antes precalcular lo que hoy lee `ROOM_MESH` en ejecución (sombras, rayos de visión, relieve, `mesh_builder`). C3 solo si P11 muestra crecimiento. |
74. **Sombras de volumen que faltan en algunos suelos y paredes** (02/11/2026): el receptor se decide por instancia (`is_room` en `M_DrawInstance`, `batcher.c`) y el volumen llega a `M_SHADOW_LENGTH` (1024) bajo el casco; no reproducido. Pedir nivel y posici�n; sospechas: suelos m�s bajos que el largo del volumen, salas bajo el agua con Lara vadeando, caras fuera de `needs_room_lights`.
76. **Probar en consola real el disco de 3 juegos** (03/11/2026, DC build 1107 en CDI, 1101 en ISO DMA): cambio TR1→TR2→TR3 (arreglados el fundido retenido y el desborde de manejadores `nolua.c`, sin ver en pantalla), cuelgue al 100 % en los menús (no reproducido en Flycast; posible DMA), informe de cuelgue con fuente BIOS, fila "Memory use", orden de lectura del ISO (`disc_sort.sh`).
77. **Medir "Load cache"** (Render, última fila): carga repetida del mismo nivel con On y Off (el mensaje "Load X.Y s" sale abajo a la izquierda). Solo cachea los archivos de inyección descomprimidos; guardar el resultado de `Inject_AllInjections` (~3,6 s) exige copiar el estado del nivel, no hecho.
78. **Conectar más mensajes a `DC_Msg()`** (consola de mensajes, `nolua_hud.c`): hoy solo la carga; pedir cuáles.
