# Plan: TRX para Dreamcast sin Lua (todo en C)

Decidido el 02/11/2026: en Dreamcast no se usa Lua. Toda la lógica y la
interfaz que hoy están en scripts se escriben en C. Rama
`refactorizacion-masiva` (worktree `D:\trx\trx2-dc-refactor`).

## Por qué

En partida Lua retiene 1,1–1,4 MB en una región fija de 1,8 MB, más 135 K de
la máquina virtual y 97 K del puente `lua/capi` (16 300 líneas en
`src/trx/game/lua`). Es la mayor partida de RAM que no es del nivel.

## Qué hace Lua hoy (scripts sin TR4)

| Parte | Tamaño fuente | Destino en C |
|---|---:|---|
| Reglas de cada juego (`games/tr*/scripts/_game.lua`) y scripts de nivel (cinemáticas, arreglos de nivel) | ~25 K | `src/platform/dreamcast/nolua/game_tr{1,2,3}.c` |
| Módulos de juego (`timer`, `water_color`, `secret_music`, `weapons`, `save_crystal`, `ngplus`, `inv_ring`, `quest_items`) | ~10 K | `nolua/modules.c` |
| HUD (`modules/ui/hud`) | 62 K | `nolua/hud.c` |
| Anillo de inventario (`modules/ui/ring`, `ring_hints`) | 84 K | `nolua/ring.c` |
| Pantallas: pasaporte, estadísticas, título, carga/guardado (`modules/ui/screens`) | 121 K | `nolua/screens.c` |
| Ajustes (`modules/ui/settings`): en DC solo la página Rendering | 128 K (DC usa una parte) | `nolua/settings.c` |
| Fundidos, rótulos, inicio de nivel, widgets | ~70 K | `nolua/ui.c` |

La interfaz en C que TRX tenía antes (borrada entre el 07 y el 17/10/2026:
`git log --diff-filter=D -- src/trx/game/ui`) sirve de punto de partida.

## Cómo

El motor solo llama a 31 funciones `LUA_*` (eventos, tick, escenas de UI,
store, scripts de nivel, hooks). Con la opción de Meson `dc_lua=false`:

- `src/trx/game/lua/*.c`, la librería Lua, `dc_lua_alloc.c` y los scripts
  incrustados quedan fuera del build.
- `src/platform/dreamcast/nolua/nolua.c` implementa esas 31 funciones. Los
  eventos van a manejadores en C (`DC_Events_Listen`), que es donde se
  enchufan las fases siguientes.

## Fases (cada una se prueba con `tools/dreamcast/test_levels.ps1`)

| Fase | Qué | Prueba |
|---|---|---|
| 0 | Build sin Lua: el juego carga y recorre todos los niveles (ganchos de prueba, sin interfaz) | TR1/TR2/TR3 `[levels] done`; RAM liberada |
| 1 | Reglas de juego y scripts de nivel en C | pasadas + cinemáticas |
| 2 | HUD en C (vida, aire, sprint, munición, recogidas, rótulos, fundidos) | visto en Flycast |
| 3 | Anillo de inventario en C | usar objetos, combinar, examinar |
| 4 | Pasaporte (guardar, cargar, nueva partida, salir), estadísticas, título | P2 de memoria (guardados) |
| 5 | Ajustes de DC (página Rendering) | cambiar modos |
| 6 | `dc_lua=false` por defecto; quitar la región de Lua | P1/P2 de los tres juegos |

## Estado

- Fase 0 (02/11/2026): hecha. `DC_NOLUA=1 bash tools/dreamcast/build.sh
  minsize` (carpeta `build/dc-minsize-nolua`); `make_disc.sh` no pone
  scripts en el disco si no hay `TRXLUA.BIN`. Código 2613 K → 2266 K (−347 K).
  TR2 recorre los 18 niveles; la región de nivel pasa de 7295 K a 9599 K
  (+2304 K: la región fija de Lua y la VM ya no existen).
- Fase 1: en curso.
  - Hecho: armas de `weapons.json5` (`nolua_weapons.c`, el lector de
    `weapons_spec.c` sobre JSON), `ngplus`, `save_crystal`, `secret_music`
    (`nolua_game.c`).
  - Hecho: los 32 scripts de nivel de TR1–TR3 (`nolua_levels.c`: aliados,
    bancos de peces, artefactos, destellos de las cinemáticas, el ventilador
    de roofs...). Los tres juegos recorren todos sus niveles (16/18/20
    cargas, sin avisos). En TR2 house la daga va a la cadera: el script Lua
    comparaba un booleano con -1 y nunca la ponía.
  - `water_color`: en DC no hace falta hasta la fase 5 (sin presets, el modo
    es "custom": el color propio del juego). Fase 1 cerrada.
- Fase 2 (02/11/2026): escrita, compila, SIN PROBAR.
  - `nolua_hud.c`: barras (vida/veneno, aire, sprint, exposición,
    enemigo/aliado), munición, FPS. Aspecto por defecto de cada juego fijo
    (tr1_pc, tr2_ps1, tr3_ps1); los demás aspectos con la fase 5.
  - `nolua_ui.c`: fundidos con retención de pantalla (`LUA_UI_*HoldingScreen`
    en nolua.c ya guarda el estado), inicio de nivel, rótulos y flechas,
    recogidas, HUD de assault (TR2 assault, TR3 house).
  - `LUA_DropLevelScript` dispara `LEVEL_UNLOAD` como `lua/scripts.c`.
  - Build tras la corrección: OK. Recorrido de niveles (código de la
    fase 2): TR1 16 y TR2 18 cargas `[levels] done`; TR3 sin leer aún.
  - Falta ver el HUD en Flycast (ventana visible: el usuario lo permitió).
- Fase 3 (02/11/2026): `nolua_ring.c` escrito, compila, SIN PROBAR. Anillo
  del inventario en C sobre `UI_TAKEOVER_MENU` y el evento TICK (entradas de
  `inv_ring.json5`, fondo, pistas, contador).
- Fase 4 (02/11/2026): `nolua_passport.c` escrito, compila, SIN PROBAR.
  Pasaporte, páginas, listas (`DC_REQ`), guardar con aviso de VMU, cristal,
  pantalla de guardado/carga rápidos. Despachador de pantallas en `nolua.c`
  (`DC_Screens_*`).
- Fase 5: `nolua_settings.c` es un trozo vacío (la entrada de gráficos no
  abre nada). Falta la página DC Rendering y los otros aspectos del HUD.
- Falta: `nolua_screens.c` (pausa, estadísticas, examinar, combinar,
  imagen, vista de nivel, globo; `DC_NoLua_SetupScreens` está declarada y
  sin definir: no llamarla), probar todo en Flycast, repetir
  `test_levels.ps1` con el build nuevo, y la fase 6 (`dc_lua=false` por
  defecto, quitar la región de Lua, pruebas de memoria).
