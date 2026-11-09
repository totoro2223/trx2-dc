# Análisis de coste de enemigos e IA en la Dreamcast (lectura de código)

Fecha: 04/11/2026. Análisis de solo lectura de `src/trx/game` y
`src/platform/dreamcast`; no se ha compilado ni ejecutado nada.

## Qué es medido y qué es estimado

- **Medido (citado de los docs)**: solo lo marcado "MEDIDO" con su fuente.
  Todo son cifras de Flycast, que no modela la latencia de `fdiv`, `fsqrt`,
  `fsrra` ni los `double` por software (PROBADO.md, filas del relieve con
  `fsrra`). En consola real lo que depende de divisiones, raíces y libm será
  peor que lo que digan esas cifras.
- **Estimado**: todo lo demás. Sale de contar operaciones en el código y de
  suponer el SH-4 a 200 MHz con 16 KB de caché de datos, sin L2, fallo de
  caché de unos 30 ciclos, división entera de 32 bits de unos 35 ciclos y
  `__divdi3` (64 bits) de más de 100. Son órdenes de magnitud para comparar
  filas entre sí, no tiempos reales.
- **Escenario de referencia** para las estimaciones: nivel de TR3, ~400
  ítems (`MAX_ITEMS` es 512 en DC, `items/const.h:8`), ~120 salas, 10
  enemigos activos (con hueco de IA), 6 de ellos dibujados, Lara con armas
  sacadas. `ITEM` mide bastante más de 200 bytes (`items/types.h:44`), así
  que recorrer ítems toca una línea de caché por ítem.
- No hay ninguna cifra medida del coste por enemigo en consola. Lo más
  cercano son las sondas `DC_PROBE_ITEMS` (todo `Item_Control`, `control.c:352`)
  y `DC_PROBE_I_LIGHT`/`DC_PROBE_O_US`, sin resultados citados en los docs
  para un nivel con enemigos. **Primer paso recomendado: medir con sondas
  por fila de esta tabla en consola antes de recortar a ciegas.**

## Flujo completo de un tick de enemigo

1. `Game_TickWorld` (`game/control.c:350`) → `Item_Control`
   (`items/manager.c:685`).
2. `Item_Control` recorre **solo la lista de simulados**
   (`Item_GetNextSimulated`/`next_simulated`), no todos los ítems. Un ítem
   inactivo (no activado por trigger) no cuesta nada en esta lista. Antes
   hace `M_SyncAllHitPoints` (todos los ítems, búsqueda de propiedad por
   nombre) y en DC ya está cacheado por `ObjectProperty_GetGeneration()`
   (`manager.c:687-703`; el comentario del código dice que costaba casi un
   milisegundo por tick). Después `M_ControlFades` recorre **todos** los
   ítems usados cada tick (`manager.c:211`) y `Carrier_AnimateDrops`.
3. Por cada criatura simulada se llama a su `control_func` (`M_Control`
   de cada objeto). Patrón común (p. ej. `mercenary.c:99-240`):
   `Creature_Activate` → (`Creature_GetAITarget` si `ai_bits != 0`) →
   `Creature_AIInfo` → `Creature_Mood` (`UpdateMood` + `ApplyMood` →
   `Box_ValidBox`/`StalkBox`/`EscapeBox`/`TargetBox` →
   `Box_CalculateTarget` → `M_UpdateLOT` → `Box_SearchLOT`) →
   `Creature_Turn` → máquina de estados (con 1–3 llamadas a
   `Creature_CanTargetEnemy` → `LOS_Check`) → `Creature_Animate`
   (`common.c:1116`: `Item_Animate`, `Item_GetBoundsAccurate`, 5–9
   `Room_GetSector`, `Box_BadFloor` hasta 3 veces, `M_GetBaddieOverlap`,
   `Room_GetHeight`/`Room_GetCeiling`, `Item_UpdateRoom`; en TR2/TR3
   `Item_UpdateRoom` también **antes** de animar, `common.c:1131-1136`).
4. Colisión con Lara: solo ítems de las salas contiguas a Lara
   (`lara/control.c:335-362`), acotada. `Creature_Collision` no es un
   problema.
5. Dibujo (por frame, 30 o 60 fps): `Interpolation_Interpolate` recorre todos
   los ítems; `M_DrawRoomItem` (`rooms/draw.c:653`) → `M_ObjectHidden`
   (5 rayos de visibilidad, solo DC) → `Object_DrawAnimatingItemWithSwap`
   (`objects/draw.c:256`) → `Output_CalculateObjectLighting` → sombra →
   `M_DrawInterpolatedObject` (`draw.c:123`) → por malla
   `Output_DrawObjectMesh_I` → `Matrix_Interpolate` → batcher DC
   (`batcher.c:5832 M_DrawInstance`).

## Tabla de costes, de mayor a menor (estimado salvo lo marcado)

Unidad: ms por tick (T) o por frame (F) en el escenario de referencia.

| # | Ruta (fichero:línea) | Qué hace | Frecuencia | Complejidad | Por qué es caro en SH-4 | Recorte DRÁSTICO propuesto | Coste est. |
|---|---|---|---|---|---|---|---|
| 1 | `matrix.c:690 M_Interpolate` → `M_InterpolateFloat:427`; llamada desde `output/draw.c:90 Output_DrawObjectMesh_I` y `anims/walk.c:146-167`; cadena de 4 matrices en `walk.c`/`matrix.c:1129 Matrix_Rot16_ID` | Cada malla interpolada de un enemigo hace 2 mezclas de matriz (vista y mundo) por cuaternión: 2× matriz→cuaternión (`sqrtf` + división), `acosf` + 3 `sinf` + división en el slerp (si el giro > ~5°), normalizar (`sqrtf` + división), ortogonalizar, 9 redondeos. Además cada hueso rota y traslada **4** matrices (`_ID`) en vez de 2. Se hace en cada malla de cada enemigo dibujado, en cada frame | por frame × enemigo dibujado × malla (~15) | O(mallas) por enemigo | `fsqrt`/`fdiv` ~13+ ciclos que Flycast no cuenta; `acosf`/`sinf` de libm (cientos de ciclos). Casi todo enemigo está entre dos keyframes (`frac != 0`, `Anim_Walk_BeginToJoint`), así que casi siempre va por esta ruta | (a) Enemigos a más de ~8 casillas de la cámara, o todos salvo los 3 más cercanos: dibujar con `Item_GetBestFrame` (un solo pose, sin `_I`, 2 matrices en vez de 4, 0 mezclas): quita el 100 % de este coste en ellos. (b) Para el resto, nlerp lineal sin `acosf/sinf` ni ortogonalizar. (c) Alternativa: mezclar solo la matriz de mundo (Lara lo intentó y falló en DC, PROBADO fila 378, no repetir tal cual) | **MEDIDO (análogo)**: Lara, mismas funciones, `lblend` 0,33 ms/frame, ~11 µs por matriz (REGISTRO.md:607, Flycast, 15 mallas). Por enemigo ≈ 0,33 ms + 4 cadenas de matriz (est.). 6 dibujados ≈ **2–3 ms/F** (Flycast; consola peor) |
| 2 | `items/manager.c:94 M_RemoveFromDrawQueues` (llamada desde `Item_UpdateRoom:949`; 117 llamadas en el código; `Creature_Animate` la llama 1–2 veces por tick) | Para quitar un ítem de las colas de dibujo recorre **todas las salas del nivel** (`Room_GetCount`) y hace un test de bit en el `drawn_items` de cada una (`rooms/draw.c:1037`). Después `M_AddToDrawQueues:164` calcula `Item_GetBoundsAccurate` (otra vez) y recorre los portales de la sala | por `Item_UpdateRoom` = por criatura y tick ×2 (TR2/3), más proyectiles y todo ítem que se mueve | O(salas) por llamada | Un acceso a una línea de caché distinta por sala (cada `ROOM` es grande, `drawn_items` está dentro): ~100–150 fallos de caché por llamada. TRX lo añadió; el original no tenía colas de dibujo por sala con bitsets | Guardar en el `ITEM` la lista de salas en las que está ya (p. ej. `int16_t draw_rooms[6]; uint8_t n`) y quitar solo de esas; si desborda, caer al bucle completo. Resultado idéntico, riesgo nulo. Extra: no rehacer las colas si la sala y la caja de ocupación no cambiaron (`M_GetOccupancyBounds` depende solo de `pos` y el radio fijo del objeto) | **0,4–1 ms/T** (20–50 µs por llamada × 10–20 llamadas) |
| 3 | `collision/los.c:535 LOS_Check` (`M_CheckX:77`, `M_CheckZ:187`, `M_GetSlope:67`, `M_ClipTargetWithSlopes:326`); llamadores `creature/common.c:980 CanSeeEnemy`, `:1014 HasLineOfFire`, `:1042 CanTargetEnemy` (123 llamadas en `objects/creatures`: hasta 9 sitios en un mismo `gunman.c`) | Rayo sector a sector en X y en Z hasta 7–8 casillas (`CREATURE_SHOOT_RANGE`); cada paso hace 2× `Room_GetSector` + `Room_GetHeight` + `Room_GetCeiling`. Las pendientes usan **división de 64 bits** (`M_GetSlope`, 4 por rayo); el recorte con pendientes hace 3 divisiones por iteración | 1–3 por tirador y tick (según estado) | O(casillas) ≈ 16–32 sectores por rayo | `__divdi3` de 64 bits en software; `Room_GetSector` con tablas de sector dispersas en memoria | (a) Caché de resultado por criatura (`los_tick`, `los_result`): reevaluar cada 4 ticks, o cuando Lara/criatura cambian de casilla; los enemigos de TR no necesitan puntería por tick. (b) `M_GetSlope`/`M_ApplySlope` en `int32` (`delta * 1024` cabe si `|delta| < 2 M`; caer a 64 bits solo si no cabe): quita las `__divdi3`. (c) Solo evaluar si `info->ahead && distance < RANGE` antes de nada (ya está en `CanTargetEnemy`, falta en `CanSeeEnemy`) | **0,5–1,5 ms/T** (30–80 µs por rayo × ~10–20) |
| 4 | `gun/misc.c:501 Gun_GetNewTarget`, 2.ª pasada `:557`; `gun/misc.c:58 M_ConsiderTarget`; `items/utils.c:86 Item_IsTargetable` | Con armas sacadas (pistolas, rifle, moto de nieve) cada tick: 1.ª pasada sobre simulados y **2.ª pasada sobre todos los ítems del nivel**. `Item_IsTargetable` hace `ObjectFamily_Has(PROJECTILE)` (búsqueda lineal en un `Vector`), luego `hit_points`, `Item_IsInPlay`, `Creature_IsHostile` (otra `ObjectFamily_Has` + `Vector_Contains` de aliados). Lo que pasa el filtro de distancia hace `Gun_FindTargetPoint` (`Item_GetBestFrame`) + `LOS_Check`; un enemigo que falla el rayo o el ángulo en la 1.ª pasada se evalúa **otra vez** (2 rayos) porque `M_TargetListContains` solo excluye los aceptados | por tick mientras Lara tiene el arma sacada | O(ítems × familia) + O(enemigos cercanos × LOS) | ~400 ítems × (línea de caché + 2–3 búsquedas en vectores) | (a) Mirar `hit_points > 0` y `is_visible` antes de `ObjectFamily_Has`. (b) Mapa de bits por `object_id` calculado al cargar el nivel: "puede ser blanco" (no proyectil; tiene `is_targetable_func` o es de familia criatura/skidoo armado). (c) Marcar en la 1.ª pasada los ítems ya evaluados para no repetir su rayo en la 2.ª. (d) Reelegir blanco cada 2–3 ticks y mantener el actual entre medias | **0,25–0,6 ms/T** (solo en combate) |
| 5 | `interpolation.c:351 M_RememberItems` (cada tick, `control.c:110`) y `:363 M_InterpolateItems` (cada frame, `game/draw.c:21`) | Recorren **todos** los ítems del nivel: copian 7 campos a `interp.prev` y interpolan (floats, `Math_AngleMean`, `Math_AngleInCone`) los activos; los inactivos solo hacen commit. Se ejecutan también a 30 fps, cuando la tasa es 1,0 y no se interpola nada útil | por tick y por frame × todos los ítems | O(ítems) | Una o dos líneas de caché por ítem; 3 ángulos con función cada uno por ítem activo | A 30 fps (`Interpolation_IsActive()` falso) saltar `M_RememberItems`/`M_InterpolateItems` y copiar `result = pos` solo en los simulados al dibujar. A 60 fps iterar la lista de simulados + los ítems que se movieron este tick (marca `moved`), no los 400 | **0,25–0,4 ms/F** (est., ~0,15 por tick + ~0,15 por frame) |
| 6 | `items/anim.c:143 Item_GetFrames`, `items/col.c:89 Item_GetBoundsAccurate`, `items/anim.c:135 Item_GetBestFrame`; llamadas: `Creature_Animate:1144`, `manager.c:137 M_IsMeshHeldAside`, `:124 M_GetMeshCenter`, `Gun_FindTargetPoint`, `CanSeeEnemy`/`HasLineOfFire` (item **y** enemigo), `Object_DrawAnimatingItemWithSwap:256-262` (otras 2) | Cada llamada repite el cálculo de keyframe: ~8 divisiones/módulos enteros por `key_frame_span`, cálculo de `frame_pos` con `TRX_REAL` (ya float), la llamada opcional a `can_interpolate_func`, y `Item_GetBoundsAccurate` suma **6 divisiones más** (`/ rate`). Una criatura lo recalcula 3–6 veces por tick con los mismos datos | por tick y criatura (3–6×) y por frame y enemigo dibujado (2×) | O(1) pero con ~14 divisiones | División entera ≈ 35 ciclos en SH-4: ~500 ciclos por llamada | Caché por ítem: `(anim_num, frame_num, prev_frame_num, alpha)` → `frames[2]`, `frac`, `rate`, `bounds`; invalidar al cambiar cualquiera. Quita 80 % de las llamadas. Además `M_IsMeshHeldAside` solo hace falta en ítems con animaciones que desplazan la malla (Lara, vehículos, trepadores): marca por objeto al cargar | **0,15–0,4 ms/T** |
| 7 | `creature/common.c:1116 Creature_Animate` (cuerpo, no las llamadas anteriores) y `box.c:597 Box_BadFloor` | 5–9 `Room_GetSector`, hasta 3 `Box_BadFloor` (cada una con otro `Room_GetSector`), `Room_GetHeight`/`Room_GetCeiling`, `M_GetBaddieOverlap:322` (recorre la cadena de ítems de la sala del enemigo), ajuste de techo en TR3, prueba de ahogo (`M_TestDrowned`, otro `Room_GetSector`) | por criatura y tick | O(1) con constante alta + O(ítems de la sala) | `Room_GetSector` recorre portales y evalúa superficies partidas | Si el enemigo no se movió este tick (`pos == old` y no hubo desplazamiento el tick anterior): saltar las pruebas de pared/`BadFloor` y `M_GetBaddieOverlap` (cambia el resultado solo en casos de empuje por una puerta o bloque, raros). `M_TestDrowned` solo si la sala es de agua o `pos.y` cambió de sala | **0,1–0,3 ms/T** |
| 8 | `pathing/box.c:84 M_UpdateLOT` → `:182 Box_SearchLOT` (tope `BOX_MAX_EXPANSION` 5, igual que los originales, no tocar), `:412 Box_CalculateTarget`, `creature/common.c:697 Creature_ApplyMood`, `:590 UpdateMood`, `:462 Creature_AIInfo` | Búsqueda de ruta (5 expansiones por tick por criatura), recorte del objetivo siguiendo la cadena `exit_box`, elección de caja, `Math_Atan` ×2 y rotación de pivote en `AIInfo`. Todo en enteros (la `Math_Atan` ya usa `fdiv` + corrección en DC, `trig.c:490`) | por criatura y tick; `ApplyMood` ya salta 1 de cada 2 ticks si el enemigo está a >6 casillas y tiene ruta (`common.c:709-720`) | O(5 × solapes) + O(longitud de ruta) | Muchos accesos a tablas de cajas (`BOX_INFO`, `BOX_NODE`) dispersas | Extender lo ya hecho: a >12 casillas de Lara, ejecutar también `Box_SearchLOT`/`CalculateTarget` solo 1 de cada 4 ticks (siguiendo el último `creature->target`); a >20 casillas congelar la criatura (no llamar a su `control_func`, ver fila 12). `LOT_InitialiseSlot` (`lot.c:187`) hace `ClearLOT` y `CreateZone` sobre **todas las cajas del nivel**: al activarse o al ser desalojada una criatura; con más de 32 activas (`LOT_SLOT_COUNT`) puede repetirse (`LOT_EnableBaddieAI:138` además recorre los huecos cada tick por cada criatura sin hueco) | **0,15–0,3 ms/T** |
| 9 | `creature/common.c:1812 Creature_GetAITarget`: bucles en `:1852`, `:1864`, `:1886`, `:1929`, `:1955`, `:1968` | Para criaturas con `ai_bits != 0` recorre `Item_GetTotalCount()` ítems mirando `object_id` y, solo en los que coinciden, `room_num`, `Creature_SameZone` (2× `Room_GetWorldSector`) y `rot.y`. **No** hay búsqueda de propiedades por nombre aquí; esa está en `FindAITargetObject`/`FindAIObjectByOCB` (fila 11). En DC, con `enemy == lara_item` solo 1 de cada 16 ticks (`:1826-1832`). Sigue completo cada tick si el objetivo no es Lara ni el marcador esperado (aliados, TR3 mono con `ai_bits == 0`: llamada incondicional en `monkey.c:215`) | por criatura con marca de IA; 1/16 ticks si apunta a Lara | O(ítems) ≈ 400 líneas de caché | `Item_Get` no está inline (llamada + comprobación de rango por ítem) y cada ítem es una línea de caché distinta | Lista de marcadores por nivel: al terminar de cargar (y en `Item_Create`/`Item_Destroy` de un `O_AI_*`) construir `m_Markers[tipo][]` con los números de ítem de cada `O_AI_*` (+ medipacks y `O_KEY_ITEM_4` del mono). El bucle pasa a O(marcadores) (0–30), y si no hay marcadores de ese tipo sale sin recorrer nada. Con eso se puede quitar el espaciado a 1/16 (que cambia el comportamiento ligeramente) | hoy ≈ 100 µs por escaneo (est.) ≈ 6 µs por criatura y tick en media; **0,05–0,1 ms/T**; en TR1 (sin marcas) 0 |
| 10 | `rooms/draw.c:382 M_ObjectHidden`, `:653 M_DrawRoomItem` | Por enemigo dibujado y frame: `sqrtf`, 2 divisiones, 5 rayos de visibilidad (`OutputSource_Rooms_RayBlocked`), y un bucle sobre las salas visibles (`Room_DrawGetCount`) con `M_DrawSet_Has` | por frame × ítem visible | O(salas visibles) por ítem + 5 rayos | `sqrtf` y divisiones en float por ítem | Ya es un recorte (Great Wall objetos 2,21 → 1,03 ms, MEDIDO en REGISTRO.md:176). Mejora: caché por ítem de "oculto" que dure 2–4 frames mientras ni cámara ni ítem cambien de sector | 0,1–0,3 ms/F (est.) |
| 11 | `creature/common.c:1771 FindAITargetObject`, `:1795 FindAIObjectByOCB` (`ObjectProperty_GetItemValue(.., "ocb", ..)` con `strcmp` lineal, `property.c:54,337`); llamadas en `guide.c:303`, `von_croy.c:626,1166`, `enemy_jeep.c:235,361`, `henchman.c:416` | Recorren **todos** los ítems del nivel y, en los que coinciden, buscan la propiedad `ocb` por nombre | cada tick, solo en esos PNJ/jefes (guía, Von Croy, jeep enemigo, algunos secuaces) | O(ítems) + `strcmp` por coincidencia | Línea de caché por ítem + `strcmp` | Usar la misma lista de marcadores (fila 9) y guardar el `ocb` en `item->ai_ocb` (el campo ya existe, `items/types.h`) al cargar; así sin `strcmp` | 0,02–0,1 ms/T, solo en niveles con esos PNJ |
| 12 | `items/manager.c:211 M_ControlFades` | Recorre todos los ítems usados cada tick solo para ver si `fade > 0` | por tick, fijo | O(ítems) | Una línea de caché por ítem solo para leer 2 bytes | Contador de ítems con `fade > 0`; si es 0, no recorrer (o lista de ítems en fundido) | **0,05–0,1 ms/T** (est.) fijos |
| 13 | `platform/dreamcast/render/batcher.c:6911 DC_Shadow_TakeObjectCaster`, `lights/common.c:237 Output_CalculateObjectLighting`, `objects/draw.c:281 OutputSource_Shadows_Draw` | Sombra de volumen de enemigos (apagada por defecto, `dc_object_shadows`; máx. 6 casters a <8 casillas de Lara, ya acotado), luz por ítem y sombra octogonal | por frame × enemigo dibujado | O(1) | — | La caché de luz por ítem se probó y no compensa (PROBADO, 0,14 ms en total, MEDIDO). Mantener `dc_object_shadows` apagado en consola; sombra octogonal ya medida (1,22 → 1,10 ms) | ≤0,15 ms/F |
| 14 | `collision/los.c:393 LOS_CheckItemIntersectSegment` y `:493 LOS_CheckSmashable` | Intersección rayo–caja con **`double`** (software en SH-4) y `cosf`/`sinf` de libm; recorre todos los ítems del nivel con 3 `ObjectFamily_Has` por ítem | por disparo de Lara (`gun/smashing.c:62`), no por tick de enemigo | O(ítems) | `double`, `sinf`/`cosf` | Pasar a `float` y usar `Math_Sin/Cos` de tabla; lista de ítems rompibles precalculada | solo en ráfagas de disparo |

### Resumen en ms/tick (escenario de referencia, estimado)

| Bloque | Estimado |
|---|---|
| Dibujo de 6 enemigos con mezcla de matrices (fila 1) | 2–3 ms/F (la única con análogo medido) |
| Colas de dibujo por sala (fila 2) | 0,4–1 ms/T |
| Rayos de visión/tiro (fila 3) | 0,5–1,5 ms/T |
| Blancos de Lara con armas (fila 4) | 0,25–0,6 ms/T en combate |
| Interpolación de todos los ítems (fila 5) | 0,25–0,4 ms/F |
| Fotogramas/cajas repetidos (fila 6) | 0,15–0,4 ms/T |
| Colisión de suelo (fila 7) y ruta (fila 8) | 0,25–0,6 ms/T |
| Resto (9–14) | ~0,3 ms |

## Mapa de recorridos sobre la lista completa de ítems

Lista exhaustiva de `Item_GetTotalCount`/`Item_GetLevelCount` en las zonas
pedidas (llamador, frecuencia, complejidad, ¿marcas de IA?):

| Sitio | Frecuencia | Coste | Notas |
|---|---|---|---|
| `creature/common.c:1826` (6 bucles de `GetAITarget`) | por criatura con `ai_bits`, 1/16 ticks en DC si apunta a Lara | O(criaturas con IA × ítems) | TR1: ninguna marca de IA, nunca se llama; TR2: pocas; TR3: muchas (guardias, patrullas, emboscadas) |
| `common.c:1774` `FindAITargetObject`, `:1798` `FindAIObjectByOCB` | cada tick, en los PNJ citados | O(ítems) + `strcmp` | solo guía, Von Croy, jeep enemigo, secuaces con OCB |
| `gun/misc.c:557` 2.ª pasada | cada tick con arma sacada | O(ítems) | todos los juegos |
| `interpolation.c:353,366` | cada tick y cada frame | O(ítems) | todos |
| `manager.c:216` (`M_ControlFades`), `:274` (`M_SyncAllHitPoints`, cacheado en DC) | cada tick | O(ítems) | todos |
| `manager.c:94` (salas, no ítems) | por `Item_UpdateRoom` | O(salas) | todos |
| `los.c:493` (`LOS_CheckSmashable`) | por disparo | O(ítems) | todos |
| `items/carrier.c:458` (`Carrier_AnimateDrops`) | cada tick solo mientras haya objetos cayendo | O(ítems) | cuerpos que sueltan objetos |
| `objects/common.c:65` (`M_FindReceptacle`), `door.c:422`, `floor_switch.c:219`, `lift.c:513`, `pickup.c:1098`, `movable_block.c:406,1154`, `raptor_emitter.c:38`, `security_laser.c:237`, `rooms/triggers/general.c:14`, `cheat.c:154`, `sophia.c:246,262`, `tribe_boss.c:114`, `winston_army.c:68`, `combat_end.c` | al cargar, por trigger o por uso de Lara | O(ítems) | No son de enemigos por tick; no tocar |
| `platform/dreamcast/vmu_hud.c:211` | solo con el buscador de secretos de la VMU activo | O(ítems) | fuera del alcance |

No hay recorridos de la lista completa en `game/camera` ni `game/lua` por tick
de enemigo (los eventos Lua solo salen al cambiar de sala, `manager.c:970`, y
DC corre sin Lua en el juego, PLAN_SIN_LUA.md).

## `double`, raíces, trigonometría, divisiones y cadenas en la ruta

- IA y animación (`creature/`, `pathing/`, `objects/creatures/` salvo
  `von_croy.c`, `items/`): **enteros**. `Math_Sin/Cos` son tablas
  (`trig.c:473`), `Math_Atan` usa `fdiv` + corrección en DC (`trig.c:490`),
  `Math_Sqrt` ya está por bits. `TRX_REAL` es `float` en DC (`func.h`).
  No hay `double` en la ruta de IA de enemigos; el comentario de PROBADO
  ("`double` a float") ya se aplicó.
- Divisiones enteras caras: `Item_GetFrames`/`Item_GetBoundsAccurate`
  (fila 6), `M_GetSlope` con **64 bits** y `M_ClipTargetWithSlopes` (fila 3),
  `Creature_ApplyMood` (`zone_count * Random / 0x7FFF`, 1 por tick),
  `Box_TargetBox` (`>> 15`, sin división).
- `double`/libm reales: `LOS_CheckItemIntersectSegment` (fila 14, `los.c:396-465`)
  y `objects/draw.c` en la función de límites (líneas ~60-100, usa `double`
  por vértice, solo para el cálculo de límites, no en el camino normal de
  dibujo). `sqrtf`/`acosf`/`sinf`: `matrix.c` `M_InterpolateFloat` (fila 1)
  y `rooms/draw.c:388 M_ObjectHidden` (fila 10).
- Cadenas: `ObjectProperty_GetItemValue` (`property.c:337`) hace `strcmp`
  lineal; en DC ya no se llama por ítem y tick para `max_hit_points`
  (`manager.c:687`). Quedan `ocb` (fila 11), `M_ReadFlag` de `carrier.c:33`
  (al soltar objetos) y `henchman.c:147` (una vez al iniciar).
  `ObjectFamily_Has` (`families.c:97`) es una búsqueda lineal en un
  `Vector` y se llama en rutas calientes: `Item_IsTargetable` (fila 4),
  `M_IsEnemy`, `Creature_Animate` ×2 en TR2/TR3, `Creature_IsHostile`.
  Convertirla en un mapa de bits `[object_id][familia]` (≤ 1 KB) la deja en
  una lectura.

## Qué hace TRX distinto de los originales

Los originales corrían en Pentium a 30 fps con 5–8 criaturas simuladas
(8 en TR1, 5 en TR2/TR3, INFORME_REA_TR123.md) y búsqueda de ruta con tope de
5 expansiones. TRX añade:

1. 32 huecos de IA (`LOT_SLOT_COUNT`, `pathing/const.h:8`) en vez de 5–8:
   hasta 4–6 veces más criaturas simuladas. `rendering.dc_original_ai`
   (`lot.c:97`) vuelve al límite original, pero está desactivado por defecto
   y es un ajuste del jugador, no un recorte. **El recorte más barato es
   cambiar el límite por defecto en DC** (p. ej. 12) y activar el desalojo por
   distancia que ya existe (`LOT_EnableBaddieAI:138`).
2. Colas de dibujo por sala con bitsets (`manager.c:94-186`, fila 2).
3. Interpolación de ítems y de huesos con cuaternión para los 60 fps
   (filas 1 y 5); en el original, 30 fps con mezcla lineal por hueso.
4. Propiedades por nombre (`ObjectProperty_*`) y familias por `Vector`
   (filas 4, 11 y el resto).
5. `Creature_FindAIObjectByOCB`, `Creature_FindAITargetObject` y los
   marcadores TR4 (`M_IsAIObject`, `common.c:295`): recorren el nivel.
6. `Creature_AlertNearbyGuards` (`creature/alert.c:6`) recorre los 32 huecos
   cuando Lara dispara (`lara/control.c:1134`, cada 128 ticks) o explota una
   granada: coste despreciable.
7. Blancos de Lara en dos pasadas (fila 4) con `Item_IsTargetable` genérico.
8. Muchos arreglos de "calidad" (`fix_enemy_route_delay`, `M_GetBaddieOverlap`
   en TR3, rotaciones extra de cabeza) que añaden trabajo pequeño por criatura.

## Orden propuesto de ataque

1. **Fila 2 (colas de dibujo)**: lista de salas por ítem. Resultado idéntico,
   sin riesgo de comportamiento, gran ganancia por criatura.
2. **Fila 1 (dibujo)**: sin mezcla de matrices y con un solo pose para
   enemigos lejanos o más allá de los N más cercanos. Es el mayor coste
   probable y el único con un análogo medido.
3. **Fila 4 (blancos)** y **fila 3 (rayos)**: bitmap de "puede ser blanco",
   marca de evaluados, caché de resultado de rayo y divisiones en 32 bits.
4. **Filas 5, 6, 12**: dejar de recorrer ítems inactivos; caché de fotogramas.
5. **Fila 9**: lista de marcadores por nivel.
6. Límite de huecos de IA por defecto más bajo en DC como palanca global.

Estado (09/11/2026): 1 hecho; 2 hecho (mezcla lineal por defecto, pila de
mundo, huesos en asm; pose única lejana solo en Original); 3 a medias (caja de
blancos y `M_GetSlope` de 32 bits; falta la caché del rayo y el `double` de
`LOS_CheckItemIntersectSegment`); 4 a medias (frames en asm y en caché;
`Interpolation_*` y `M_ControlFades` siguen recorriendo todo); 5 a medias
(escaneo cada 16 ticks); 6 descartado (desincroniza las demos). Las filas 7-8
pasaron a asm. Medido en Flycast (demo 1 de TR1: el coste está en dibujar);
nada medido en consola.

Antes de aplicar nada, medir en consola con sondas por fila (`DC_PROBE`)
sobre un nivel de TR3 con 8–10 enemigos activos: ninguna de las cifras
estimadas de este documento está comprobada.

## Qué no se ha mirado

- Lo que hacen `control_func` concretos por tipo de enemigo más allá del patrón
  común (jefes, vehículos, secuaces con equipo).
- `OutputSource_Rooms_RayBlocked` (coste interno de `M_ObjectHidden`).
- `Sound_*`, `Spawn_*` (sangre) y `Item_TakeDamage` al recibir disparos.
- El coste real de `Room_GetSector`/`Room_GetHeight` en consola; se ha supuesto
  "caro" por su recorrido de portales y superficies partidas.
