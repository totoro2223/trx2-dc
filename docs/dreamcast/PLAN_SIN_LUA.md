# Plan: TRX para Dreamcast sin Lua (todo en C)

Decidido el 02/11/2026: en Dreamcast no se usa Lua. Toda la lÃ³gica y la
interfaz que hoy estÃ¡n en scripts se escriben en C. Rama
`refactorizacion-masiva` (worktree `D:\trx\trx2-dc-refactor`).

## Por quÃ©

En partida Lua retiene 1,1â€“1,4 MB en una regiÃ³n fija de 1,8 MB, mÃ¡s 135 K de
la mÃ¡quina virtual y 97 K del puente `lua/capi` (16 300 lÃ­neas en
`src/trx/game/lua`). Es la mayor partida de RAM que no es del nivel.

## QuÃ© hace Lua hoy (scripts sin TR4)

| Parte | TamaÃ±o fuente | Destino en C |
|---|---:|---|
| Reglas de cada juego (`games/tr*/scripts/_game.lua`) y scripts de nivel (cinemÃ¡ticas, arreglos de nivel) | ~25 K | `src/platform/dreamcast/nolua/game_tr{1,2,3}.c` |
| MÃ³dulos de juego (`timer`, `water_color`, `secret_music`, `weapons`, `save_crystal`, `ngplus`, `inv_ring`, `quest_items`) | ~10 K | `nolua/modules.c` |
| HUD (`modules/ui/hud`) | 62 K | `nolua/hud.c` |
| Anillo de inventario (`modules/ui/ring`, `ring_hints`) | 84 K | `nolua/ring.c` |
| Pantallas: pasaporte, estadÃ­sticas, tÃ­tulo, carga/guardado (`modules/ui/screens`) | 121 K | `nolua/screens.c` |
| Ajustes (`modules/ui/settings`): en DC solo la pÃ¡gina Rendering | 128 K (DC usa una parte) | `nolua/settings.c` |
| Fundidos, rÃ³tulos, inicio de nivel, widgets | ~70 K | `nolua/ui.c` |

La interfaz en C que TRX tenÃ­a antes (borrada entre el 07 y el 17/10/2026:
`git log --diff-filter=D -- src/trx/game/ui`) sirve de punto de partida.

## CÃ³mo

El motor solo llama a 31 funciones `LUA_*` (eventos, tick, escenas de UI,
store, scripts de nivel, hooks). Con la opciÃ³n de Meson `dc_lua=false`:

- `src/trx/game/lua/*.c`, la librerÃ­a Lua, `dc_lua_alloc.c` y los scripts
  incrustados quedan fuera del build.
- `src/platform/dreamcast/nolua/nolua.c` implementa esas 31 funciones. Los
  eventos van a manejadores en C (`DC_Events_Listen`), que es donde se
  enchufan las fases siguientes.

## Fases (cada una se prueba con `tools/dreamcast/test_levels.ps1`)

| Fase | QuÃ© | Prueba |
|---|---|---|
| 0 | Build sin Lua: el juego carga y recorre todos los niveles (ganchos de prueba, sin interfaz) | TR1/TR2/TR3 `[levels] done`; RAM liberada |
| 1 | Reglas de juego y scripts de nivel en C | pasadas + cinemÃ¡ticas |
| 2 | HUD en C (vida, aire, sprint, municiÃ³n, recogidas, rÃ³tulos, fundidos) | visto en Flycast |
| 3 | Anillo de inventario en C | usar objetos, combinar, examinar |
| 4 | Pasaporte (guardar, cargar, nueva partida, salir), estadÃ­sticas, tÃ­tulo | P2 de memoria (guardados) |
| 5 | Ajustes de DC (pÃ¡gina Rendering) | cambiar modos |
| 6 | `dc_lua=false` por defecto; quitar la regiÃ³n de Lua | P1/P2 de los tres juegos |

## Estado

- Fase 0 (02/11/2026): hecha. `DC_NOLUA=1 bash tools/dreamcast/build.sh
  minsize` (carpeta `build/dc-minsize-nolua`); `make_disc.sh` no pone
  scripts en el disco si no hay `TRXLUA.BIN`. CÃ³digo 2613 K â†’ 2266 K (âˆ’347 K).
  TR2 recorre los 18 niveles; la regiÃ³n de nivel pasa de 7295 K a 9599 K
  (+2304 K: la regiÃ³n fija de Lua y la VM ya no existen).
- Fase 1: en curso.
  - Hecho: armas de `weapons.json5` (`nolua_weapons.c`, el lector de
    `weapons_spec.c` sobre JSON), `ngplus`, `save_crystal`, `secret_music`
    (`nolua_game.c`).
  - Hecho: los 32 scripts de nivel de TR1â€“TR3 (`nolua_levels.c`: aliados,
    bancos de peces, artefactos, destellos de las cinemÃ¡ticas, el ventilador
    de roofs...). Los tres juegos recorren todos sus niveles (16/18/20
    cargas, sin avisos). En TR2 house la daga va a la cadera: el script Lua
    comparaba un booleano con -1 y nunca la ponÃ­a.
  - `water_color`: en DC no hace falta hasta la fase 5 (sin presets, el modo
    es "custom": el color propio del juego). Fase 1 cerrada.
- Fase 2 (02/11/2026): escrita, compila, SIN PROBAR.
  - `nolua_hud.c`: barras (vida/veneno, aire, sprint, exposiciÃ³n,
    enemigo/aliado), municiÃ³n, FPS. Aspecto por defecto de cada juego fijo
    (tr1_pc, tr2_ps1, tr3_ps1); los demÃ¡s aspectos con la fase 5.
  - `nolua_ui.c`: fundidos con retenciÃ³n de pantalla (`LUA_UI_*HoldingScreen`
    en nolua.c ya guarda el estado), inicio de nivel, rÃ³tulos y flechas,
    recogidas, HUD de assault (TR2 assault, TR3 house).
  - `LUA_DropLevelScript` dispara `LEVEL_UNLOAD` como `lua/scripts.c`.
  - Build tras la correcciÃ³n: OK. Recorrido de niveles (cÃ³digo de la
    fase 2): TR1 16 y TR2 18 cargas `[levels] done`; TR3 sin leer aÃºn.
  - Falta ver el HUD en Flycast (ventana visible: el usuario lo permitiÃ³).
- Fase 3 (02/11/2026): `nolua_ring.c` escrito, compila, SIN PROBAR. Anillo
  del inventario en C sobre `UI_TAKEOVER_MENU` y el evento TICK (entradas de
  `inv_ring.json5`, fondo, pistas, contador).
- Fase 4 (02/11/2026): `nolua_passport.c` escrito, compila, SIN PROBAR.
  Pasaporte, pÃ¡ginas, listas (`DC_REQ`), guardar con aviso de VMU, cristal,
  pantalla de guardado/carga rÃ¡pidos. Despachador de pantallas en `nolua.c`
  (`DC_Screens_*`).
- Fase 5 (02/11/2026): `nolua_settings.c` escrito, compila, SIN PROBAR. PÃ¡gina
  Rendering de DC: modo (aplica sus valores por defecto), reiniciar nivel,
  frame rate, cel shading, sombras, objetos 3D, relieve, cel de salas,
  contador de FPS, filtros de textura y de interfaz. Izquierda/derecha o
  confirmar cambian el valor. Faltan los otros aspectos del HUD.
- `nolua_screens.c` (02/11/2026): pausa, estadÃ­sticas (una sola caja, sin
  pÃ¡gina de assault), imagen y vista de nivel en C. Compila, SIN PROBAR.
  Globo, examinar y combinar no estÃ¡n definidos (el motor sale al tÃ­tulo y
  el anillo devuelve la entrada).
- Fase 6 (02/11/2026): `dc_lua=false` es el valor por defecto (meson.options)
  y `build.sh minsize` compila sin Lua en `build/dc-minsize`; `DC_LUA=1`
  compila el juego con scripts en `build/dc-minsize-lua`. Sin Lua no hay
  regiÃ³n de Lua. Compila, SIN PROBAR. La carpeta `build/dc-minsize-nolua` ya
  no se usa.
- Probado 02/11/2026 (Flycast oculto): recorrido de niveles de TR1/TR2/TR3 y P2 de TR2 OK sin Lua. Visto en Flycast (TR2 nivel 1, ventana de prueba): FPS, anillo de inventario, anillo de opciones, pasaporte (cargar/guardar/salir) y página Graphic Options con cambio de valor, OK. Sin ver: HUD de vida/aire con enemigo, examinar/combinar, guardar real, TR1/TR3, y el estilo del HUD elegible.

