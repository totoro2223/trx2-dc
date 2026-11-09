# Plan ASM total

Pasar a ASM (SH-4) todo el camino de dibujado, geometría y lógica de juego que
sigue en C. 9 fases, 36 pasos. Orden de ejecución: 1, 2, 3, 5, 6, 7, 8, 9 y,
por último, la parte de colisión de la fase 4.

**Estado (09/11/2026):** hechos en ASM los pasos 1, 3, 9, 13, 20, 21 y 22; hechos sin verify corrido 2, 18 y 19; a medias 4, 5, 6 (la emisión por store queue no: los vértices por SQ fallaron en consola), 7, 8, 11, 12, 14, 17, 24, 26-29 y 35; sin empezar 10, 15, 16, 23, 25, 30-34 y 36. Fuera del plan: `dc_sqrt32`, `dc_sin`, `dc_atan`, `dc_distance`, `dc_chunk_hidden` (compilado fuera con `TRX_DC_OCCLUDE 0`). Todo con el C de referencia (`TRX_DC_ASMOFF`/`ASMOFF2`, `DC_EMIT_VERIFY`); nada probado en consola. Antártida en Flycast: 35,9 fps todo en ASM frente a 36,2 en C (PROBADO). Detalle por rutina en MIGRACION_ASM.md.

Reglas:

- Cada paso lleva su bit en `TRX_DC_ASMOFF` para apagarlo si algo falla en
  consola.
- Comprobación: comparar la salida de la rutina ASM con la de C en una escena
  fija antes de borrar la C. Es una comparación de resultados, no una medición
  de velocidad.
- Al terminar cada fase: fila en `PROBADO.md` y se quita la tarea de
  `TAREAS.md`.
- Compila con `minsize` tras cada cambio.
- Todo debe valer para TR1, TR2 y TR3.

## Fase 1: transformación
1. `M_XformOwn`, bucle por vértice: variante ASM que lee las banderas
   (`NO_LIGHTING`, `NO_FOG`, `OWN_LIGHT`, `BILLBOARD`, `ABS_SPRITE`) y salta a
   la rama correcta, sin exigir banderas uniformes.
2. `M_XformGeneric` (floats): variante ASM con banderas mixtas, luz de sala y
   de objeto. Cubre billboard bajo cel y ortho.
3. `M_TransformInstance` queda sin ruta en C: solo despacha a ASM.

## Fase 2: coste fijo por instancia
4. `M_Stage`: copia de matriz y relleno de la instancia en ASM (`movca.l`,
   prefetch).
5. `M_PrepareInstanceLight`, tinte y línea de agua: una rutina ASM, un pase.
6. Emisión de caras (`M_EmitChunk`, `M_DrawInstance`): bucle ASM a la Store
   Queue, con vértices compartidos y caras gemelas resueltos en la pasada.
7. `M_EnsureBuilt`: volcado de vértices en ASM. Construcción anticipada
   también para puertas, trampas y cuerpos.

## Fase 3: partículas y sprites
8. `OutputSource_Sprites_Stage`: usa el camino de partículas
   (`DC_MeshBatcher_StageParticle`) con la proyección en ASM.
9. Nieve y partículas restantes: una llamada ASM por grupo.

## Fase 4: sombra, colisión y animación de criaturas
10. Sombra del juego (`game_shadow`): proyección y emisión del cuadrilátero.
11. Colisión (`Room_GetSector`, `Room_GetHeightEx`, `M_GetSurfaceHeight`) y
    `LOS_Check`. Estado global (`m_HeightType`, `Camera_IsChunky`) en
    registros o en una struct fija. Va al final de todo. Leer el código entero
    antes de escribirlo.
12. `Collide_GetSpheres` y `Creature_Animate`.

## Fase 5: visibilidad
13. Recorrido de salas y portales: proyección, recorte y cola de salas.
14. Culling por frustum de salas y objetos: esfera y caja contra seis planos,
    en lote.
15. Selección de qué se dibuja: filtro de mallas y objetos visibles y orden
    por distancia.

## Fase 6: pasadas del batcher
16. `M_RenderPass` y reparto por páginas de textura y modo (opaco,
    punch-through, transparente).
17. Cel: umbral de sombreado por cara.
18. Bump: normales y alfa de bump por cara cercana.
19. Reflejos (env-map): UV de entorno por vértice.
20. Sombra de volumen: extrusión de siluetas y emisión de volúmenes.

## Fase 7: agua, niebla y flipmaps
21. Agua y cáusticas: ondulación de vértices y UV animadas.
22. Niebla de sala: color de niebla por vértice (`M_FogColours` y la de sala).
23. Flipmaps: conmutación de salas y rebuild de geometría.

## Fase 8: animación e interfaz
24. Interpolación de animación de objetos (`M_DrawInterpolatedObject`) y
    mezcla de huesos.
25. HUD, texto y 2D: emisión de quads.

## Fase 9: lógica de enemigos y objetos
Orden dentro de la fase: 9A, 9C, 9B y 9D. Cada función pasa a ASM tal cual,
rama por rama, sin cambiar la lógica.

9A. Núcleo compartido de criaturas
26. `Creature_Animate`, `Creature_Mood`, `Creature_AIInfo`, `Creature_Target`,
    `Creature_Turn`, `Creature_Kill`, `Creature_Collision`.
27. Pathing y casillas: `lot.c` (`LOT_*`), búsqueda de ruta, cajas de
    navegación.
28. Colisión de objetos y enemigos: `Collide_*`, empuje contra Lara y paredes,
    `Item_Collision`, `Item_UpdateRoom`.
29. Animación de items: `Item_Animate`, avance de frame, comandos de
    animación, `Anim_*`.

9B. Controles por tipo de enemigo
30. TR1: lobo, oso, murciélago, cocodrilo, león, pantera, gorila, rata,
    velocirraptor, T-Rex, Midas, Natla y demás.
31. TR2: tigre, perro, ave, matón, buzo, Dragón y demás.
32. TR3: Antártida, India, Londres, Nevada, Pacífico y jefes.

9C. Objetos y trampas
33. Puertas, trampillas, interruptores, plataformas y bloques móviles.
34. Trampas: pinchos, sierras, bolas, flechas, dardos y lava.
35. Proyectiles y efectos: balas, bolas de fuego, explosiones, sangre, humo.

9D. Lara
36. Control de Lara (`lara/`: movimiento, agua, escalada, armas) y sus
    comprobaciones de colisión.

## Riesgo
La fase 9 y el paso 11 tocan la jugabilidad de TR1, TR2 y TR3. Van al final y
cada grupo se apaga por separado con `TRX_DC_ASMOFF`.
