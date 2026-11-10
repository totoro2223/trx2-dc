# Tareas pendientes

Solo lo que falta. Lo hecho y lo descartado está en [PROBADO.md](PROBADO.md)
(resumen) y [REGISTRO.md](REGISTRO.md) (detalle): **míralos antes de empezar**
para no repetir una idea que ya falló. Entorno, ciclo de compilación y reglas
con Flycast: [TRASPASO.md](TRASPASO.md).

Lo hecho sin medir o sin ver está en [PRUEBAS_PENDIENTES.md](PRUEBAS_PENDIENTES.md).

Actualizado el 09/11/2026 (revisión de lo pendiente contra el código).

**Memoria en sesiones largas** (niveles seguidos, cargar/guardar, cambio de
juego, reinicio de nivel): plan completo de pruebas P1–P13 y correcciones
M1–M13 en [PLAN_MEMORIA_SESIONES_LARGAS.md](PLAN_MEMORIA_SESIONES_LARGAS.md).
Cubre las filas 64, 66, 67 y 70 de la sección J (la 65 está resuelta; la 68,
hecha sin probar).

**Refactor para bajar la RAM** (02/11/2026: revisión del fork OpenLara-dc,
reparto de la RAM, fases A–C con ganancias estimadas): ver
[PLAN_REFACTOR_RAM.md](PLAN_REFACTOR_RAM.md).

**Geometría que no se ve** (objetos tapados por paredes, trozos más finos en
salas grandes, tope del relieve): plan G1-G4 en
[PLAN_GEOMETRIA_OCULTA.md](PLAN_GEOMETRIA_OCULTA.md). G1 (objetos tapados,
`M_ObjectHidden` en `rooms/draw.c`) y G2 a 256 hechos el 30/10 sin medir;
toda la oclusión por rayos (salas, trozos, objetos) va apagada en compilación
desde el 03/11 (`TRX_DC_OCCLUDE 0`, `batcher.c`); trozos más finos (1024/512)
FALLIDO (PROBADO); G3 y G4 sin empezar.

**VRAM libre** (bytecode de Lua a VRAM, mipmaps del mundo, mapa de sombra
suave de 128): plan V1–V3 en
[PLAN_VRAM_LUA_MIPMAPS_SOMBRA.md](PLAN_VRAM_LUA_MIPMAPS_SOMBRA.md).
V1 (Lua en VRAM) OK en Flycast, sin uso en la build por defecto (sin Lua). V3: a 128 faltan caras de las salas (ver PROBADO); hoy 96 en Soft (high) y 64 en Soft (`5c6e7f989`), sin ver. V2 (mipmaps) apagado por defecto: corrompe las páginas con texels transparentes en Flycast; `TRX_MIP=1` en `texvq` los activa para investigar. Borrar esta nota cuando se resuelvan.

**REA TR1, TR2 y TR3** (04/11/2026): análisis de los exe originales hecho;
cifras en [INFORME_REA_TR123.md](INFORME_REA_TR123.md) (plan de partida:
[PLAN_REA_TR123.md](PLAN_REA_TR123.md)). Lo que se aplica al port está en la
tarea 79; lo que quedó sin resolver, en la 80.

Al terminar una tarea: anota la fila en PROBADO.md (OK o FALLIDO, con cifra y
build), el detalle en REGISTRO.md, y **bórrala de aquí**. Retira las sondas
`TEMP` (`dc_probe.h` y los `DC_PROBE(...)`) antes de dar un commit por bueno.

Reglas comunes: cambios en código compartido bajo `#if defined(__DREAMCAST__)`;
nada de `build/`, CDIs ni datos del juego en git; no tocar un Flycast abierto
por el usuario; decir siempre "DC build N"; lo que no gane nada medible se
deshace y se anota como FALLIDO. **La niebla no se toca** (regla del usuario,
05/11/2026): la del original (PSX: desde 12 casillas, corte en 20); ninguna
optimización puede acortar la distancia de dibujo.

---

## A. Comprobar lo ya compilado (prioridad alta)

2. **Ver a ojo Soft (high) y Soft** en TR2 y TR3 (Soft (high) con celdas de
   96 desde `5c6e7f989`; deformación de UV en suelos grandes con luces
   cercanas) y medir su coste frente a Hard (visto en Flycast el 08/11,
   PROBADO).
4. **Ver en pantalla** lo de la lista "sin ver" de PROBADO.md (sombra bajo el
   agua, sangre y bengala de TR3).
6. **Medir el coste del relieve con brillo** en Londres frente al relieve
   anterior. El ajuste a ojo ya está hecho (PROBADO, 27/10/2026); valores de
   hoy: `M_BUMP_DEPTH` 0,765, `M_BUMP_GLOW` 0,5, luces de paso 8× de peso y
   1,5 de alcance, apagado bajo 15 fps (`M_BUMP_MIN_FPS`).

## B. Consola real

5. **Probar en GDEMU y medir.** Flycast no modela las cachés del SH-4 (16 KB de
   datos, 8 KB de instrucciones). Hasta entonces, nada que dependa de la caché
   se puede dar por bueno ni por malo: `M_EmitChunk`, `movca.l`, descarte de
   espaldas por plano, datos calientes/fríos del vértice de sala. Con
   `dc_profile` y `prof_report.py` solo hay una cota optimista. Ya medido en
   consola: Jungle 17-19 fps (DC build 1219/1221), demo 1 de TR1 17-18 fps;
   falta lo que depende de la caché. (Las store queues, antes tarea 71:
   FALLIDO en consola, PROBADO.)

## C. Optimización del render (lo que de verdad queda)

- Migraciones a ASM: lo hecho, lo pendiente por coste medido y cómo se verifica están en
  [MIGRACION_ASM.md](MIGRACION_ASM.md) (Antártida sigue en 36-37 fps).
- Ideas de render sin empezar (09/11/2026: recorrer solo vértices únicos,
  color final guardado, niebla de la PVR por tabla, `.DCL`, media caché como
  RAM, vértices de intensidad, descarte por la PVR), con lo que ya está hecho
  y su coste estimado: [optimizacion render.md](optimizacion%20render.md).
- **60 fps fijos en consola** (09/11/2026, rama `full60fps`, hecho y probado
  en Flycast): modo «Native 60 FPS» con salas por lotes en la OCRAM, la lógica
  junto a un hilo de render, niebla por tabla de la PVR y tope de polígonos.
  Falta medir en GDEMU (Antártida, Londres, Jungle, demo 1 de TR1) con la
  vista 2 del HUD, mirar la imagen, y probar el inventario y el guardado con el
  hilo: [PLAN_60_FIJO.md](PLAN_60_FIJO.md).

Reparto de Londres (graphics, ms por frame, 23/10, antes de quitar Lua): pass
~12,7 (estáticos y sprites 3,2, emisión 3,0, objetos 1,6, salas 1,5), stage
~7, Lua ~2,6, lógica ~4,2.

9. **30 reservas por frame** de `Memory_Alloc` (`Catalog_CreateKey`,
   `Memory_DupStr`, `String_Format`). Medido con Lua: volver a medir sin él.
10. **`Sparks_Control`**: ya en ASM (`sparks_ctl_sh4.S`, PROBADO 06/11); lo
    que queda es `Random_GetControl` y las luces dinámicas por chispa.
11. **Emisión** (3,0 ms): es volumen de instrucciones repartido; bajar más pide
    menos caras miradas o vértices ya listos para el PowerVR. Sin pista barata.
12. **Mezcla de matrices de Lara** (`lblend` 0,33 ms): el batcher de DC solo
    lee la pila de mundo (PROBADO 06/11, `g_MatrixWorldOnly` en enemigos);
    probar lo mismo en el dibujo de Lara. Vale ~0,16 ms.
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

17. **TR3**: salas de color sin el mínimo de sombra ni el parpadeo. (Chispas
    con giro, rectangulares e interpoladas: hechas en ASM, PROBADO.
    Guardados en la VMU: comprobado.)
18. **Experimental 60**: franja negra arriba a 320×240; comprobar la vuelta a
    640×480. (El define `TRX_DC_MMU_NULL_TRAP` ya no está en `meson.build`.)
19. **Inventario**: 9 → 22 fps con Lua; sin medir con el anillo en C
    (`nolua_ring.c`).
20. **Audio**: reverb y filtro bajo el agua (programa propio del DSP del AICA).
73. **FMV MPEG-1 en consola: tirones los primeros 30 s** (reproductor
    reescrito, DC builds 948-956: OK en Flycast y sincronizado en consola,
    PROBADO). Hipótesis sin probar: leer con `setvbuf` de 64 KB. Falta oír el
    sonido. (Las películas ya se reproducen antes de cargar el nivel:
    `sequencer.c`, PROBADO.)
21. **Deseos del usuario**: botón para la bengala (L+Y es agacharse en TR3;
    hoy sin asignar, `controller.c`), textura del cielo, efectos,
    Controles/Juego en el menú. (Hechos: L como modificador, Sonido con
    Soundtrack, FMV, armas largas a la espalda, todos los niveles de TR1, TR2
    y TR3.) Aparcado: pantalla Rendering mínima de DC, mipmaps del mundo
    (J74), subir `vertex_buf_size` de PVR (512 K).

## F. Bug aparcado

22. **Heap corrupto en la demo del Ganges (TR3)**. El usuario cree que es de
    esa demo y no vale la pena seguir. Solo reabrir si aparece en otro nivel.
    Reproducir: `TRX_DC_GAME=tr3 TRX_DC_DEMO=<n>` en `make_disc.sh`.

## G. Stubs que afectan a TR1–TR3

El motor compartido ya decide qué dibujar; lo que falta en Dreamcast es el
destino de dibujo: funciones vacías `DC_STUB()` en
`src/platform/dreamcast/render/stubs.c` (30, 12 de ellos `TRX_GL_*`) y `av_stub.c` (vídeo y
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
| 31 | **Depuración** (`Misc_StageSphere`, `RoomsDebug_*`; `Misc_StageCuboid` hecho, T78): esferas, ascensor, puertas de disparo, `lua/capi/scene.c` | sí | sí | sí | Baja: no son efectos del juego |

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
| 64 | **Memoria en sesiones largas: batería cerrada** (P1, P2, P5 OK). El guardado de Barkhang y la carga de TR3 ya están arreglados y el detalle está en [REPORTE_MEMORIA.md](REPORTE_MEMORIA.md). | P2 TR3 completo 0–18 pasa con `[levels] done` (build 804, 66 guardados / 19 cargas, log `build/logs/2026-10-28/dc-mem-P2-tr3e-fresh.txt`). Al cargar el nivel 19 hay `Out of memory` de newlib y watchdog pero acaba pasando. Falta P3 (cargas repetidas), P4 (pico de Barkhang con guardados), P8 (reiniciar/cambiar de juego), P11 (sesión larga). Antes de dar el commit por bueno, quitar las sondas TEMP `[save]` (`savegame/file.c`) y `[arena]` (`dc_save_arena.c`). |
| 66 | **Caché de transparencia de objetos** (02/11: ya en memoria para cargas repetidas, falta la primera carga) (`object_transparency_*.cache.dat`): el disco RAM no tiene directorios y nunca se escribe, así que cada carga vuelve a dibujar todas las mallas para hallarla (y hace crecer las matrices del batcher en el pico). | Generarla al hacer el disco (`make_disc.sh`) o guardarla en `/ram` a secas. |
| 67 | **MIGRACIÓN IMPORTANTE DE LÓGICA: memoria por arenas con época y zona estática acotada** (anotada el 24/10/2026; no empezada, para hacer en otra ocasión). Es la única vía sistemática para "esterilizar" la RAM al cargar un nivel, salir al menú o cambiar de juego sin reiniciar la consola: un limpiador general no existe porque el motor guarda punteros directos (no se puede compactar), y limpiar solo libera lo que alguien liberó. Pasos: **(1)** que todo el motor reserve desde arenas con "época" (permanente, partida y nivel; hoy solo el nivel lo hace) para soltar la arena entera al terminar una partida o cambiar de juego; **(2)** que todas las variables estáticas del motor queden en una zona de memoria acotada con el enlazador (secciones de datos y bss propias), para ponerla a cero o restaurarla entera al cambiar de juego (hoy cada módulo se limpia a mano: `m_IsExiting`, `PhaseExecutor_Reset`, los interruptores de las pruebas, `TRX_DIR`; se escapan los que no se conocen); **(3)** dejar fuera de esa zona lo de KallistiOS y newlib, porque sus drivers siguen vivos (sonido, vídeo, mandos) y tocar su estado los rompe. Esfuerzo de varios días y riesgo alto: un puntero estático que se escape queda colgando. **Lo que queda aparte y no resuelve esta migración:** el pico de carga de TR2 Barkhang y TR3 (tareas 64), las 287–299 K que cada juego deja retenidas, la fragmentación del montón de newlib por objetos pequeños (Lua, cadenas) y salir al selector de niveles, que todavía reinicia el disco. | Decidirlo con un caso real delante (qué falla, en qué juego y nivel). Medir antes con `live_census.py` qué retiene cada juego. |
| 68 | **Sin efectos de sonido tras cambiar de juego**: causa hallada, `Audio_Shutdown` no liberaba los canales del AICA que reserva `Audio_Init` (`66aa9f940`, solo compilado). | Cambiar de juego varias veces en Flycast y en consola y oír los efectos. |
| 69 | **Deriva del hueco mayor de la región por carga** (~90 K por carga en TR1, build 320: 6271 → 5448 K en 13 cargas, con solo 3 bloques retenidos): la piscina de 256 K se llenaba durante la carga, lo que sobraba se apilaba bajo el muro y el mapa de solapes de salas nuevo caía cada vez más abajo. Piscina a 384 K, sin probar (tarea A1). El mapa ya se libera en `Level_Unload` (C4, 02/11/2026, sin probar). | Medir la deriva con la piscina de 384 K y C4 (TR1, 13 cargas; `Room_ReleaseOverlapMap` ya va en `Level_Unload`). Si sigue bajando: reservar el mapa en la cabeza (fuera de la ventana del nivel). |
| 70 | **Las 287–299 K que cada juego deja retenidas** al cambiar de juego en el mismo proceso (T70) | Con `-Ddc_heap_census=true` y `live_census.py` tras un cambio de juego. |
| 71 | **Sombra suave más oscura**: hecho MAX 0,95 y MIN 0,7 (sin luz ≈0,78, `ce18f7d4a`, sin ver). Falta `M_SOFT_SELF_STRENGTH` (Lara sobre sí misma) 0,7 → 0,75. | Constante en `batcher.c`. Mirar junto al cel shading y anotar en PROBADO. |
| 72 | **Transparencias en negro y agua negra de TR2** (consola real): los texels con color clave salían negros en la lista punch-through en las 4 variantes (builds 909-911); hoy van mezclados por la lista translúcida (`m_PtVariant` 3, `pvr.c`; L+R+Izquierda cambia de variante). Queda: en TR2 el fondo del agua y la superficie vista desde abajo salen negros (con bengala se ve el relieve encima), y lo que siga negro con la variante 3. | En consola, disco con `-Ddc_bg_magenta=true`: ¿huecos o caras negras? Mirar con una traza qué mezcla y página lleva lo que sale negro. |
| 73 | **Mapa de sombra**: 128 FALLIDO (PROBADO); hoy 96 en Soft (high) y 64 en Soft (`5c6e7f989`), sin ver. | Ver en Flycast que no faltan caras de las salas de TR1 con Soft (high). |
| 74 | **Mipmaps VQ**: corrompen en Flycast las páginas con texels transparentes (Lara, fuente, pasaporte), aun con el sesgo en 0,25. Los datos (TVQ2) decodifican bien. | Comparar con la disposición real del PVR/Flycast; probar una página con alpha sola. `TRX_MIP=1` activa la generación. |
| 75 | **Plan de RAM pendiente** ([PLAN_REFACTOR_RAM.md](PLAN_REFACTOR_RAM.md), 02/11/2026): **probar** lo aplicado sin pruebas (A2, A3, C1/C2, D2 reducido, D7 parcial, C4; ver REGISTRO) y hacer lo que falta: **B** (niveles horneados `.DCL`), **D1** (una copia de cada sala), **D2 completo** (sectores decodificados al consultar), **D6** (un ejecutable por juego), **D7** (tabla de solo lectura para el hash del catálogo, tras el censo A5) y **C3** (arena de frame). | D1 exige antes precalcular lo que hoy lee `ROOM_MESH` en ejecución (sombras, rayos de visión, relieve, `mesh_builder`). C3 solo si P11 muestra crecimiento. |
74. **Sombras de volumen que faltan en algunos suelos y paredes** (02/11/2026): el receptor se decide por instancia (`is_room` en `M_DrawInstance`, `batcher.c`) y el volumen llega a `M_SHADOW_LENGTH` (1024) bajo el casco; no reproducido. Pedir nivel y posición; sospechas: suelos más bajos que el largo del volumen, salas bajo el agua con Lara vadeando, caras fuera de `needs_room_lights`.
76. **Probar en consola real el disco de 3 juegos** (antes también J63): cambio TR1→TR2→TR3 y vuelta al selector en el mismo encendido (sin `arch_reboot` desde `0660cdef5`), informe de cuelgue con fuente BIOS, fila "Memory use", orden de lectura del ISO (`disc_sort.sh`). Menús e imágenes ya OK en consola (build 1201, PROBADO; el cuelgue al 100 % era la alineación de la arena).
77. **Medir "Load cache"** (Render, última fila): carga repetida del mismo nivel con On y Off (el mensaje "Load X.Y s" sale abajo a la izquierda). Solo cachea los archivos de inyección descomprimidos; guardar el resultado de `Inject_AllInjections` (~3,6 s) exige copiar el estado del nivel, no hecho.
78. **Conectar más mensajes a `DC_Msg()`** (consola de mensajes, `nolua_hud.c`): hoy la carga y el cambio de modo; pedir cuáles.
79. **Aplicar al port las cifras de los exe originales** (REA, 04/11/2026, [INFORME_REA_TR123.md](INFORME_REA_TR123.md)). Estado 09/11: (a) probado como "Enemy AI: Original" y retirado el 04/11 (las demos se desincronizaban; hoy 32 huecos siempre); (c) hecho solo en modo Original (`rooms/draw.c`); (b), (d), (e) sin hacer. Texto original: **(a)** bajar `LOT_SLOT_COUNT` en DC de 32 a 8, o por versión con `g_TRVersion` (8 en TR1, 5 en TR2/TR3), con 8 bytes por caja y hueco (1000 cajas: 64 K frente a 256 K); medir con `gbuf_summary.py` y comprobar que ningún nivel de TR1–TR3 se queda sin enemigos; reproducir el robo del hueco del enemigo más lejano si falta. **(b)** TR3 tiene además 12 huecos ligeros sin nodos de búsqueda (flag &4 del objeto): ver cuántos enemigos de TRX son de ese tipo y si pueden dejar de gastar hueco pesado. **(c)** Tope explícito de 128 entradas con comprobación en la lista de salas visibles (los originales no comprueban). **(d)** Comprobaciones baratas en lo que los originales no comprueban (500 sonidos al cargar, 33 objetivos de disparo). **(e)** Comprobar que la tabla de texturas de DC admite 4096 (TR3). No tocar el tope de expansión: 5 en los tres originales, igual que `BOX_MAX_EXPANSION`.
80. **REA: lo que quedó sin resolver** (INFORME_REA_TR123.md, apartado final): capacidad real del array de salas visibles (TR1/TR2 estimada en 100–112, TR3 sin determinar); arrays de partículas, splash, ripples, blood, bubbles y gunflash; tope de objetos activos y de cajas; reinicio del asignador en TR2; luces, sombras y objetos por sala. Hacerlo solo si (a)–(e) de la tarea 79 no bastan.
81. **Probar en consola el ajuste "Enemy AI" (TRX / Original)** y medir (informe: [INFORME_IA_ORIGINALES_VS_TRX.md](INFORME_IA_ORIGINALES_VS_TRX.md)): mismo nivel con enemigos, cambiar solo ese ajuste, "Restart level", comparar fps y las sondas `DC_PROBE` por fila. Si Original gana de verdad, decidir si pasa a ser el valor por defecto en DC (o por juego). Probar también **Pose blend: Linear** (mezcla lineal de matriz como los originales, ya implementada, apagada por defecto): mirar a ojo que Lara y los enemigos se vean bien en giros grandes y medir el pass. Luego, por orden: dibujar a los enemigos lejanos con un solo pose (`Item_GetBestFrame`), lista de salas por ítem (`draw_rooms`), visibilidad cacheada y divisiones de 32 bits en `LOS_Check`, no recorrer ítems a 30 fps en `Interpolation_*`/`M_ControlFades`. Los marcadores de IA no son el cuello (ANALISIS_COSTE_IA_TRX.md, fila 9). Desde el 04/11 el modo Original ya no toca la IA (32 huecos y atajos de TRX, para que las demos terminen en su sitio) y conserva la mezcla lineal, la pose única lejana, el tope de 128 salas y los frames cacheados (recortes 5, 7 y 9); los exactos 8 y 10 actúan siempre. SH4ZAM está en el slerp de float (`dc_shz.c`), sin medir. Pendiente: comprobar con el log `[lpos]` (solo con `-DTRX_DC_LPOS`) que Lara termina igual en los dos modos. Ya hechos: pose única lejana, caché de `LOS_CheckSmashable`, mezcla lineal en ASM; falta medir en consola. Si algo se ve mal, es lo primero a sospechar.
83. **Trozos de sala finos** (05/11/2026): probado y descartado (celda 1024 y caja de 8 esquinas contra la ventana: más lento, -2 % de vértices; ver PROBADO.md). Queda: copia de vértices compartidos en `xform` (3932 por fotograma, ~1,2 ms; la emisión leería `vref[corner]` en `LOAD_CORNER` de `emit_sh4.S` y en las ~60 lecturas de `xv[]` en C) y aligerar el camino asm del vértice.
85. **Bengala y armas solo inyectadas en Dreamcast** (06/11/2026): el arreglo de las armas de dos manos conserva las mallas de los objetos que ya trae el nivel, pero `lara_flare` (bengala, disponible) y las armas que solo existen en `lara_guns.bin` (magnums/autos/m16 de TR3, magnums/desert eagle/mp5/cohete de TR2, etc.) solo tienen mallas de relleno (20 vértices, bloque) porque el modelo real está en `lara_outfits.bin`, que no cabe en el disco. Comprobar la bengala en TR1/TR2/TR3 (`TRX_DC_GIVE=99`) y decidir: usar el modelo del inventario pegado a la mano (como el arma de la espalda, `M_SetBackGunFromModel`) o extraer de `lara_outfits.bin` solo el objeto de intercambio del traje clásico en una inyección pequeña. Las armas no disponibles dadas con `TRX_DC_GIVE` no son un caso real.

86. **Cargas: lo que queda** (09/11/2026; medido en Flycast, TR3 `sewer.tr2` 5,5 s, `title.tr2` 2,6 s, TR2 `boat.tr2` 4,5 s, TR1 `level2.phd` 7,5 s y su título 2,9 s; ver PROBADO.md). Reparto de los 5,5 s de TR3: leer el nivel 1,5 s (salas 0,36 s, 13 páginas 0,25 s, mallas 0,15 s), inyecciones leídas 0,84 s, aplicadas 1,44 s, preparar texturas 0,75 s (relieve), el resto 0,6 s. Pendiente: **(a)** TR1: sus niveles y su título no se benefician de nada de lo hecho (páginas de 8 bits que pasan por la paleta, inyecciones de TR1 aparte): medir sus fases con `[loadtime]` y aplicar lo mismo; **(b)** `M_InitialiseInjection` descomprime cada inyección una vez para contar y `Inject_AllInjections` otra (el flujo reinicia al volver atrás; ~0,4 s en TR3): contar al hacer el disco no es sencillo (también fija `m_MaxStaticObject*`, `m_RoomMeta`, símbolos y muestras), conservar lo descomprimido gasta memoria que no sobra; **(c)** los 13 ms por página que cuesta leer su VQ (`M_LoadVQ`, un `fread` por página) y el relieve (`textures.bump`, 0,75 s): leer varias páginas seguidas de una vez; **(d)** sonidos de `winston_model.bin` (0,19 s, codifica a ADPCM al cargar): llevarlos ya codificados al disco; **(e)** medir en la consola real cuánto pesa cada fase, porque el coste allí es el disco. Carga mínima por nivel (no cargar `winston_model.bin` donde no sale): sin saber en qué niveles de TR3 aparece Winston no se hizo.

87. **Selector de banda sonora Original / Tomb Raider Suite** (aparcada el 09/11/2026, revertida): hecha y guardada en [features/soundtrack-suite.md](features/soundtrack-suite.md) con su parche; el audio quedó como antes. Pendiente para retomarla: las ambientales (pista 57 de TR1) y las pisadas de Lara no sonaron en la prueba de TR1, sin error en el registro; sin resolver.
