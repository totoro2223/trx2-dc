# Enemigos e IA: originales de TR1, TR2 y TR3 frente a TRX en Dreamcast

Fecha: 04/11/2026. Fuentes: análisis estático de los exe originales con REA
(`tomb.exe` 3Dfx, `Tomb2.exe`, `tomb3.exe`), lectura del código de TRX
([ANALISIS_COSTE_IA_TRX.md](ANALISIS_COSTE_IA_TRX.md)) y
[INFORME_REA_TR123.md](INFORME_REA_TR123.md). **Nada está medido en consola**:
las cifras de coste de TRX son estimaciones por lectura de código y Flycast no
modela `fdiv`, `fsqrt` ni los `double` del SH-4. Los nombres de funciones son
nuestros (Ghidra solo da `FUN_`).

## Conclusión corta

Por criatura, la IA de TRX hace casi lo mismo que la de los originales
(AIInfo, mood, `UpdateLOT` con tope 5, animación con suelo). Lo que cambia es
**cuántas criaturas** se simulan y dibujan a la vez, y **lo que TRX añade
alrededor**. La búsqueda de marcadores de IA **no es el problema**.

## Cómo funcionaban los originales (REA)

| | TR1 | TR2 | TR3 |
|---|---|---|---|
| Criaturas con IA a la vez | **8** | **5** | **5** (+12 ligeras) |
| Quién se simula | solo la lista activa (`next_active`) | ídem | ídem |
| Activación | solo por trigger, **nunca por distancia** | ídem | ídem |
| Sin hueco libre | roba el del más lejano a Lara; la víctima **no actúa ni se dibuja** (flags&6==6) | ídem | ídem |
| Elección de enemigo | siempre Lara; sin marcadores | switch por objeto; 4 objetos usan `ChooseEnemy` sobre **las 5 ranuras**; el resto, Lara | marcadores sí (ver abajo) |
| Búsqueda de marcadores | no existe | no existe | `GetAITarget` 0x416b60 |
| Búsqueda de ruta | `UpdateLOT` ≤5 cajas, solo si cambia la caja destino | ≤5 cajas por tick | ≤5 cajas por tick |
| Suelo por criatura y tick | ~3–9 `GetFloor` | ~5–9 | ~4–6 + hasta 4 `ShiftCheck` |
| Colisión entre criaturas | sí, solo ítems de la **misma sala** | no | solo ítems de la misma sala |
| Línea de visión por criatura y tick | no | no (aritmética sobre cajas; rayos solo al disparar) | no verificado |
| Animación en el dibujado | interpola la pose **mezclando linealmente los 12 valores de cada matriz** (`0x2b9dc`, llamada desde `0x2a648`): promedio si el paso es 2, división entera si no | ídem (`0x41ba60`): `GetFrames` 0x41bf70 devuelve fracción 0 en un keyframe y entonces no interpola; sin cuaterniones, sin trigonometría | ídem (`0x4296c0`): **solo desplazamientos de bits** (paso 2: promedio `>>1`; paso 4: cuartos `>>2`), sin división; test de bbox antes de tocar huesos |
| Dibujo | solo salas visibles; salta ítems con flags&6==6 | ídem | ídem; sin tope por frame |

**Marcadores en TR3** (0x416b60, 19 llamadores, una llamada por tick y
criatura): sin `ai_bits` sale al instante (casi todos los enemigos); con
marcador ya elegido, solo tres comparaciones de proximidad; si no, bucle
lineal sobre todos los ítems con corte en el primer acierto. Es decir, el
original tenía el mismo diseño que TRX. TR1 y TR2 no tienen nada de esto.

## Dónde TRX se separa de los originales

1. **Número de criaturas: 32 contra 5–8.** Es el multiplicador de todo lo
   demás (IA, suelo, ruta, animación, dibujado). Con 20 enemigos cerca el
   original simulaba y dibujaba 5–8; TRX, 20. Ya hay un interruptor:
   **"Enemy AI" en el menú Gráficos** (ver abajo).
2. **Interpolación de pose por malla** (`matrix.c` `M_Interpolate`): dos
   mezclas por cuaternión con `sqrtf`, `acosf`, 3 `sinf` y divisiones por
   cada malla de cada enemigo dibujado. **Los tres originales también
   interpolan, pero con una mezcla lineal de los 12 valores de la matriz** (en
   TR2/TR3 con restas, multiplicación y división entera, o solo con
   desplazamientos de bits cuando el paso entre keyframes es 2 o 4), sin
   cuaterniones ni `sqrtf`/`acosf`/`sinf`. Es la fila de mayor coste estimado (2–3 ms por frame con 6 enemigos,
   con el análogo medido de Lara: 0,33 ms por frame en Flycast).
3. **`Item_UpdateRoom` → `M_RemoveFromDrawQueues`** (`items/manager.c:94`):
   recorre todas las salas del nivel por criatura y tick (dos veces en
   TR2/TR3). No existe en el original. Est. 0,4–1 ms por tick.
4. **`LOS_Check` por criatura** (1–3 rayos por tirador y tick, con `__divdi3`
   de 64 bits en `M_GetSlope`). En TR2 el original usa aritmética sobre cajas.
   Est. 0,5–1,5 ms por tick.
5. **Pasadas sobre todos los ítems por tick** que el original no tenía:
   `Interpolation_Remember/Interpolate` (también a 30 fps, donde no
   interpolan nada útil), `M_ControlFades`, 2.ª pasada de `Gun_GetNewTarget`.
6. **Repetición de cálculo**: `Item_GetFrames` e `Item_GetBoundsAccurate`
   3–6 veces por criatura y tick, con ~14 divisiones enteras cada una.
7. **Solo DC**: `M_ObjectHidden` lanza 5 rayos de visibilidad por enemigo y
   frame (ya es un recorte de dibujado, pero cuesta).
8. **Marcadores (`Creature_GetAITarget`)**: igual que el original de TR3, con
   salida inmediata sin `ai_bits` y espaciado a 1/16 ticks en DC. Est.
   ~0,05–0,1 ms por tick. `FindAIObjectByOCB`/`FindAITargetObject` (con
   `strcmp` por nombre) solo los usan guía, Von Croy, jeep enemigo y algunos
   secuaces. **No es donde se va la CPU.**

## Recortes, de más a menos efecto esperado

Estado (09/11/2026): aplicados 2 (mezcla lineal, por defecto), 3 (`draw_rooms`,
`items/manager.c`), 4 en parte (división de 32 bits en `M_GetSlope` y filtro de
caja de los blancos; falta la caché del rayo y el mapa de bits) y 6 (frames en
asm y en caché); 5 y 7 sin hacer. Nada medido en consola. Medir antes y
después con sondas `DC_PROBE` en consola.

1. **Modo "Enemy AI: Original"** (hecho, sin probar): 8 huecos en TR1 y 5 en
   TR2/TR3, sin los atajos de IA de DC. Recorta a la vez IA, suelo, ruta,
   animación y dibujado de todo lo que sobra. Es la palanca grande y se
   compara en la misma consola cambiando solo ese ajuste.
2. **Sustituir el slerp por la mezcla lineal de los originales** en
   `M_Interpolate` para los enemigos (12 valores de matriz; paso 2 = promedio
   con `>>1`, paso 4 = `>>2`, resto = una división entera). Es lo que hacen los
   tres juegos originales y se ve igual que ellos. Corrige lo que se creyó antes
   (que TR1 no interpolaba enemigos): sí interpola. Variante más drástica:
   dibujar a los lejanos con un solo pose (`Item_GetBestFrame`), con saltos
   visibles entre keyframes.
3. **Lista de salas por ítem** (`draw_rooms[6]`) para quitar el recorrido de
   todas las salas en `M_RemoveFromDrawQueues`. Mismo resultado exacto.
4. **Visibilidad cacheada**: resultado de `LOS_Check` reutilizado 3–4 ticks y
   divisiones de 64 a 32 bits; mapa de bits de "puede ser blanco" y no
   evaluar dos veces al que falla el rayo.
5. **No recorrer ítems a 30 fps** en `Interpolation_*` y `M_ControlFades`
   (contador de ítems en fundido).
6. **Calcular una vez por criatura y tick** `Item_GetFrames` y los límites.
7. Marcadores: dejarlos como están, o precalcular al cargar una lista de
   marcadores por tipo, solo si una medición los señala.

No tocar: el tope de 5 de `UpdateLOT` (igual que los tres originales).

## Qué no se pudo comprobar

- Tiempos reales: ninguna cifra de TRX está medida en consola.
- TR1: la cadencia exacta de 30 Hz no se leyó; el dibujado de enemigo
  (0x252b4) y el init de criatura (0x205d0) salen de bytes sueltos.
- TR2: no se localizó `AnimateItem`.
- TR3: no se verificó si hay LOS por criatura y tick (las rutinas de control
  de enemigos no están definidas como funciones en Ghidra); los bits 0x400,
  0x1000 y 0x2000 de `ai_bits` y los ids de marcador son inferidos del código.
- Si a 30 fps la interpolación de mallas de TRX se salta o corre: por
  verificar antes de aplicar el recorte 2.

## Interruptor "Enemy AI" (DC build tras la 1107, sin probar)

Fila nueva en Gráficos: `TRX` (por defecto) u `Original`. Se guarda en la
VMU (formato 9) y se aplica **al cargar el nivel** (usar "Restart level").
(Desde el 04/11/2026: `Original` = mezcla lineal, una pose a más de 14
casillas, tope de 128 salas y frames cacheados; la IA es la de TRX, 32 huecos,
para que las demos acaben en su sitio. En Flycast, TR3 Londres 31,3 frente a
30,7 fps.) Antes: `LOT_SLOT_COUNT` efectivo de 8 (TR1) o 5 (TR2/TR3), sin los
dos atajos de IA de DC (pensar cada 2 ticks a más de 6 casillas; marcadores a
1/16 ticks) y nodos de ruta solo para los huecos usados. No reproduce los 12
huecos ligeros de TR3. Código: `pathing/lot.c` (`LOT_IsOriginalSim`),
`creature/common.c`, `dc_settings.c`, `nolua_settings.c`.

## Interruptor "Pose blend" (sin probar)

Fila en Gráficos: `Linear` (por defecto desde el 05/11/2026, `dc_linear_pose`; una VMU guardada antes del formato 10 lo deja en `Slerp`) o `Slerp`. `Linear` mezcla los 12 valores de cada matriz como los originales (`M_InterpolateLinear` en `matrix.c`), sin cuaterniones ni trigonometría. Se aplica al instante, a todo lo que pase por `Matrix_Interpolate` (enemigos, Lara, armas). Se guarda en la VMU (formato 10). Los originales hacían lo mismo, pero una mezcla lineal de rotaciones encoge un poco la matriz si los dos keyframes están muy separados: mirar a ojo los giros grandes de Lara.

## Recortes 3 a 10 aplicados (sin probar en consola)

Compilan sin avisos. Los marcados "Original" solo actúan con **Enemy AI: Original**; los exactos (mismo resultado) actúan siempre.

| # | Recorte | Dónde | Cuándo actúa |
|---|---|---|---|
| 3 | Sin los 5 rayos de visibilidad para criaturas e ítems (los estáticos conservan los suyos); el test de caja `M_GetClip` ya existía | `rooms/draw.c` `M_ObjectHidden` | Original |
| 4 | Fracción 0 no interpola: ya estaba (`Anim_Walk_BeginToJoint` exige `frac != 0`) | `anims/walk.c` | siempre |
| 5 | Enemigo a más de 14 casillas dibuja el keyframe más cercano, sin mezcla | `objects/draw.c` `M_DrawInterpolatedObject` | Original |
| 6 | Robo del hueco del más lejano, víctima oculta: ya lo hace `LOT_EnableBaddieAI` con 8/5 huecos | `pathing/lot.c` | Original |
| 7 | Tope de 128 salas visibles | `rooms/draw.c` `Room_MarkToBeDrawn` | Original |
| 8 | Salas por ítem (hasta 8) en vez de recorrer todas; se invalida en cada nivel y cada flip | `items/manager.c`, `rooms/common.c` | siempre (exacto) |
| 9 | `Item_GetFrames` una vez por animación, frame, frame previo y alfa | `items/anim.c` | Original |
| 10 | `M_GetSlope` con división de 32 bits si cabe (mismo resultado) | `collision/los.c` | siempre (exacto) |

Riesgos a mirar: enemigos tras una pared que ahora se dibujan si su sala es visible (3); saltos de pose de los lejanos (5); salas visibles que desaparecen en vistas muy grandes (7).

## Cambio del 04/11/2026: Original ya no toca la IA

Con **Enemy AI: Original** la IA es la de TRX (32 huecos, mismos atajos que en TRX): así una demo termina donde debe. Lo que queda en Original: mezcla lineal de poses (en TRX sigue el slerp), una sola pose para enemigos a más de 14 casillas, tope de 128 salas visibles y frames de ítem calculados una vez. Se quitó el recorte 3 (sin rayos de visibilidad): en TR1 no bajó los polígonos y solo dibujaba más. Los recortes 8 (salas por ítem) y 10 (división de 32 bits en LOS) siguen activos siempre.

Mediciones en Flycast oculto (Graphics, 60 fps; informes en `benchmarks/`): TR3 Londres, TRX 31,3 fps y Original 30,7, sin diferencia en enemigos (~0,45 ms de 32). TR1 Vilcabamba, TRX 42,0 y Original 40,6, pero con la IA anterior de Original (8 huecos) las dos demos veían salas distintas (3 contra 4), así que no son comparables. Se añadió un log TEMP `[lpos]` (posición de Lara cada 15 ticks en demos, `game/control.c`) para comprobar la sincronía; sin probar tras igualar la IA.

## SH4ZAM

Instalado como paquete de kos-ports (`libsh4zam.a`, MIT). Sus cabeceras son GNU C y el motor usa c2x, así que se compilan aparte (`dc_shz.c` en `dc_shz_lib`, con envoltorios `DC_Shz_InvSqrt`, `DC_Shz_Acos`, `DC_Shz_Sin`). Usado en el slerp de float de `matrix.c` (normalización y arcos). Sin medir: Flycast no modela `fsca`/`fsrra`. En la fase `pass` aporta poco: la transformación y la emisión ya van en ensamblador.
