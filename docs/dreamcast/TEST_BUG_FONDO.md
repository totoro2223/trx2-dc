# Test bug fondo (imágenes de copyright y menú matan la consola)

**Estado (09/11/2026): resuelto** en DC build 1201 (`al.iso`, consola real),
commit `0660cdef5`. Causa: `Memory_ArenaAlloc` no alineaba; la ruta de la
imagen en el disco multijuego (`/cd/g1/…`, 3 letras más) dejaba desalineado
el puntero de la orden de dibujo siguiente y la SH-4 fallaba (Flycast tolera
el acceso desalineado). En DC los bloques se redondean a 8 (`core/memory.c`).
Las herramientas PICMODE, NOPICS y STEPS se retiraron en ese commit; quedan
`DC_Trail` (rastro del informe de cuelgue, `dc_strict.c`), `TRX_DC_NOBG` en
`make_disc.sh` y `dc_lua_stubs.c`. La tabla de ISOs de abajo ya no hace falta.
`arch_reboot`: el cambio de juego sigue sin reiniciar la memoria
(`dc_main.c`); en Flycast, 7 cambios seguidos con la memoria retenida plana
(PROBADO); sin sesión larga en consola. Lo de abajo queda como historia.

Estado a DC build 1184 (4/11/2026). ISOs para DreamShell, con DMA. Flycast no
reproduce el fallo.

## Síntoma

- En consola real, el copyright (`eidos`) y el fondo del menú del anillo
  (`title_*`) reinician el ISO (a veces la consola entera). Muere al dibujar,
  no al leer: `pic: closed` se alcanza.
- Con `NOPICS.TXT` (sin imágenes) el menú funciona (build 1184,
  `TRX-tr1-dma-sinimagenes3.iso`).
- La imagen de la pantalla de carga se dibuja bien: misma textura (`m_Picture`),
  mismo `DC_PVR_PICTURE`, un rectángulo de 640x480.

## Lo que ya se sabe

- Diferencia entre lo que funciona y lo que falla: la carga dibuja en un
  fotograma propio, sin prueba de profundidad ni modo fondo, z = 0.1. El
  copyright y el menú dibujan dentro de la escena (`Output_BeginScene`), con
  prueba de profundidad, modo fondo y z = 0.0.
- Dibujar dentro de la escena con el estilo de la carga reinició la consola
  entera (build 1180): no repetir.
- Nombres largos: descartado (máximo 15 caracteres en el disco, todas las
  imágenes de 524288 bytes).
- Lo último que cambió: pantallas en C en lugar de Lua (commit 1d29417c2,
  2/11) y los fondos de `extras` para `title_ub`, `title_eu_gm`, `title_eu_la`
  (`make_disc.sh`, líneas 382-394). El ISO 1158 que arrancaba se hizo después
  del cambio a C; no se sabe si mostraba fondo.
- Partir la imagen en 4 trozos de 256x256: no tiene sentido, la textura de
  512x512 se dibuja bien en la carga.

## Isos hechos (DC build 1192, con DMA, arrancan en el selector de niveles)

Todos llevan el escuchador de errores de la PowerVR (`DC_Crumb_WatchPvr`): un
error sale en pantalla (borde magenta + texto), en el serie y en el VMU
(`TRXLOG`, que sobrevive al reinicio de la consola). El tiempo máximo de
espera ya existía: el vigilante de 20 s (`DC_STALL_SECONDS`) no mostró nada
en los fallos, lo que apunta a un reinicio y no a un cuelgue.

Nombres cortos por DreamShell. Salen de `build/dc-release-dma/` (`fC.iso` también copiado ahí). `fC` usa dos funciones de relleno (`dc_lua_stubs.c`): la build con Lua ya no enlazaba sola.

| ISO | Qué cambia | Qué demuestra |
|---|---|---|
| `fA.iso` | Plan A: sin los fondos de `extras` (`TRX_DC_NOBG=1`) | Si funciona, la causa son los fondos nuevos |
| `fB1.iso` | Imagen sin prueba de profundidad (`PICMODE.TXT` = 1) | Si el fallo es la prueba de profundidad |
| `fB2.iso` | Imagen a la profundidad de la pantalla de carga, z = 0.1 (`PICMODE` = 2) | Si el fallo es el valor de z |
| `fB3.iso` | Imagen sin modo fondo (`PICMODE` = 3) | Si el fallo es el modo fondo |
| `fC.iso` | Plan C: compilación con Lua (`DC_LUA=1`, `build/dc-release-dma-lua/`) | Si el cambio a pantallas en C fue el culpable |

`fB1` a `fB3` cambian una sola cosa cada uno respecto al dibujo original.
El intento que cambiaba las tres a la vez (build 1180) reinició la consola.

Flycast (con `dc_strict` y la memoria sucia) dibuja bien el fondo y el
copyright: el fallo solo se ve en la consola real.

## Isos a hacer (en este orden, una prueba cada vez)

Comando base (PowerShell, en `TRX\`):

```powershell
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh release && bash tools/dreamcast/make_disc_all.sh build/dc-release-dma build/dc-release-dma/<nombre>.iso"
```

Con `DC_ISO_DMA=1`, `TRX_DC_ALL_GAMES=1`, `TRX_DC_ALL_START_GAMES=1` y
`TRX_DC_ALL_START` sin definir (empieza en el selector de niveles).

Cinco planes. Cada uno es un ISO aparte y responde una pregunta distinta.

| Plan | ISO | Qué cambia | Qué demuestra | Estado |
|---|---|---|---|---|
| A | `fondo-originales.iso` | `title_ub` y `eidos` sin los fondos de `extras` (saltar el bloque de `make_disc.sh` 382-394, p. ej. con `TRX_DC_NOBG=1`) | Si funciona, la causa son los fondos nuevos | **Hacer primero** |
| B | `fondo-escena.iso` | Estado que deja `Output_BeginScene` antes de `M_DrawOp_Background` (estado PVR, viewport, lista de la TA); dibujar el fondo antes de la escena | Si el fallo es del contexto de la escena | Si A falla igual |
| C | `fondo-lua.iso` | Compilar `1d29417c2^` (Lua, 2/11) con DMA | Si el cambio a pantallas en C fue el culpable | Si B no aclara |
| D | `fondo-fotograma.iso` | Fondo en un fotograma propio, como la pantalla de carga | Solo muestra el fondo un fotograma: el anillo se dibuja en el siguiente y lo reemplaza. Habría que dibujar el menú entero en ese fotograma | Poco útil |
| E | `fondo-trozos.iso` | Imagen partida en 4 de 256x256 con la ruta de texturas normal | Rodea la ruta de la imagen de 512x512, pero la carga la dibuja bien, así que no ataca la causa | Descartado por ahora |

Los planes D y E los dejo anotados para no repetirlos: no los recomiendo.

## Para comprobar en cada ISO

- Copyright (`eidos`) aparece y pasa al menú.
- El menú del anillo muestra su fondo.
- "Return to title" desde un nivel vuelve al menú del anillo sin reiniciar.
- Elegir "title" desde el selector de niveles tras la primera carga.

## Herramientas de diagnóstico (TEMP, retirar antes del commit)

- `TRX_DC_STEPS=1` (STEPS.TXT): cada paso en pantalla durante 0,5 s. Con él
  arranca la demo del título antes de tiempo.
- `TRX_DC_NOPICS=1` (NOPICS.TXT): ni carga ni dibuja imágenes.
- Rastro `DC_Trail` en `pvr.c`, `sequencer_misc.c`, `common.c`, `executor.c`,
  `phase_picture.c`.

## Pendiente aparte

- Con `arch_reboot` quitado de `dc_main.c`, la memoria no se reinicia entre
  juegos al volver al título: vigilar falta de RAM tras varias vueltas.
