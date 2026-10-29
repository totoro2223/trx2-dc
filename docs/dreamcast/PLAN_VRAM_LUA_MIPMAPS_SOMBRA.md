# Plan: bytecode de Lua a VRAM, mipmaps del mundo y mapa de sombra mayor

Preparado el 29/10/2026. Tres tareas independientes que usan la VRAM libre
(~3 MB tras las páginas VQ). Antes de empezar, lee [TRASPASO.md](TRASPASO.md)
y [PROBADO.md](PROBADO.md). Cada tarea se compila, se mide y se anota por
separado (fila en PROBADO, detalle en REGISTRO). Orden recomendado: **V3 → V2
→ V1** (de menos a más trabajo).

Presupuesto de VRAM estimado (sin medir): V3 +192 KB, V2 ~+100–300 KB,
V1 ~+300 KB. Total ≲ 0,8 MB. Se mide con el `pvr_mem_available()` que ya
escribe `pvr.c:380`, en el nivel más pesado de cada juego (Venice en TR2, el
mayor de TR3), y siempre en `minsize`.

**Relación con las sombras de volumen de nivel (plan futuro)**: no chocan. V3
no corre a la vez (es el modo de sombra suave). V2 abarata el relleno. V1
libera RAM principal, útil para cachear volúmenes por sala. Solo cuenta el
presupuesto: las sombras de nivel pueden pedir subir `vertex_buf_size` (512 K
×2) en 0,5–1 MB.

---

## V3. Mapa de sombra suave de 64 a 128 por ranura

**Estado de partida**: `DC_PVR_SHADOW_MAP_SIZE 64` y
`DC_PVR_SHADOW_MAP_SLOTS 8` (`render/pvr.h:63-65`). Una sola textura RTT de
512×64 (64 KB), RGB565 no twiddled, creada en `DC_PVR_Init` (`pvr.c:473`) y
renderizada con `pvr_scene_begin_rtt` (`pvr.c:548`).

**Cambio**:

1. `DC_PVR_SHADOW_MAP_SIZE` a 128 → textura de 1024×128 (256 KB). 1024 es el
   ancho máximo del PVR, y Flycast quiere el RTT ancho (memoria
   `pvr-rtt-limits`), así que la forma vale.
2. Revisar en `batcher.c` todo lo que esté en píxeles del mapa y no en
   fracción de ranura: `batcher.c:6631` (`size`), `6838-6839` (`w`, `h`),
   y cualquier margen, desenfoque o desplazamiento de medio texel del soft
   shadow. Lo que use `1.0f / DC_PVR_SHADOW_MAP_SLOTS` (`batcher.c:4030`) ya
   escala solo.

**Medir**: `[frame]` con sombra suave en una sala con 3 luces (las 3 ranuras
de luz + las de rebanadas), antes y después, y `m_MapWaitUs`. **Criterio**:
pérdida ≤ 0,5 ms por frame; si no, probar 96 (no es potencia de 2: no vale) o
dejar 128 solo con `M_SOFT_MAP_EVERY 2`.

**Riesgo**: el pase RTT rellena 4× píxeles. Lara es pequeña en el mapa, pero
el fondo se limpia entero.

---

## V2. Mipmaps de las páginas del mundo

**Estado de partida**: ninguna textura tiene mipmaps (TAREAS, sección E, fila
21: aparcado). Páginas de 256×256 twiddled: ARGB1555 en VQ (18 KB, hechas por
`tools/dreamcast/texvq.c` en `make_disc`) o ARGB4444/1555 sin comprimir
(128 KB). Cabecera en `pvr.c:1765-1780`. Existe la "media página"
(`m_Half`, `pvr.c:120-130`) para el texto de la interfaz a 320×240: es un
mipmap a mano de un nivel, y queda como está.

**Cambio**:

1. **`texvq.c`**: generar la cadena de mipmaps de cada página VQ (256 → 1) y
   escribirla con el formato VQ mipmapeado del PVR: codebook de 2 KB y luego
   los índices de cada nivel de menor a mayor, con el relleno que el PVR
   espera para los niveles de 1×1 y 2×2. **Comprobar ese formato** en KOS
   (`pvr_texture.c`/documentación del PVR) y con una página de prueba antes
   de cambiar todas. El codebook se comparte entre niveles. Subir la versión
   del archivo a "TVQ2" y que el juego acepte las dos.
2. **Bleed del atlas**: cada página de TR junta muchas texturas pequeñas.
   Al reducir, cada nivel debe promediar **dentro de cada textura de objeto**
   (sus límites salen de las object textures del nivel), no a través de los
   bordes. Si no basta, usar el ajuste de D del PVR (`mipmap_bias` en el
   contexto del polígono) para quedarse en niveles más nítidos.
3. **Páginas sin comprimir** (ARGB4444 y las cambiadas en juego): mipmaps
   hechos en el juego al confirmar la página (`DC_PVR_CommitTexturePages`),
   igual que el bump. Se puede dejar para una segunda fase.
4. **`pvr.c`**: en la cabecera de la página, `cxt.txr.mipmap =
   PVR_MIPMAP_ENABLE` (y `mipmap_bias` si hace falta), solo si la página
   tiene cadena, con filtro bilineal **sin** trilinear (el trilinear dobla el
   relleno). La dirección de la textura apunta al codebook como hoy.
5. Solo páginas de sala y objetos del mundo; la interfaz y el texto no.

**Medir**: VRAM libre en el nivel más pesado; `[frame]` en una sala grande con
mucha profundidad (debería igualar o mejorar); capturas a distancia en
Flycast **con filtrado activado** (Flycast solo usa mipmaps con filtro,
TRASPASO:205). En consola real funciona también con filtro nearest.
**Criterio**: sin bajar fps, sin colores de otra textura en bordes lejanos.

**Riesgo**: en Flycast con filtro "Original" (nearest) no se ve nada; la
prueba visual tiene que hacerse con filtro bilineal o en GDEMU.

---

## V1. Bytecode de Lua embebido → VRAM

**Estado de partida**: `tools/embed_trx_lua.py` (desde `src/meson.build:163`)
genera `trx_embedded_lua.c` con dos tablas `LUA_EMBEDDED_SCRIPT {path, data,
size}` (`trx/game/lua/embedded_scripts.h`). Unos ~300 KB de arrays
`m_TrxLua_*` en `.rodata` (`build/symsum.sh`, minsize). Los usa
`trx/game/lua/common.c`:
- `M_LoadTRXModules` registra cada módulo como preload con
  `lua_pushlightuserdata(data)` (carga perezosa: el puntero se lee cuando el
  módulo se pide, quizá mucho después del arranque);
- `M_RunTRXRuntimeScripts` hace `luaL_loadbufferx` de cada script (en DC se
  saltan los `commands/`).

El ejecutable entero está en RAM, así que esos 300 KB ocupan RAM principal
toda la partida, aunque Lua ya haya copiado lo que necesita a su heap.

**Cambio**:

1. **Generador**: en DC (opción de meson nueva, apagada por defecto en otras
   plataformas para no tocar tests ni PC), `embed_trx_lua.py` escribe un
   blob `TRXLUA.BIN` con todo el bytecode seguido, y el `.c` solo con las
   tablas `{path, offset, size}`. Los `commands/` ni entran en el blob.
2. **Disco**: `make_disc.sh` copia `TRXLUA.BIN` junto a los datos.
3. **Arranque**: antes de iniciar Lua, `pvr_mem_malloc` del tamaño del blob,
   leerlo del disco con un buffer de rebote de 16 KB en RAM y copiarlo a VRAM
   con escrituras de 32 bits (el PVR no admite escrituras de 8 bits). Rellenar
   `data = base + offset` de cada entrada. **Comprobar el orden**: hoy
   `DC_PVR_Init` lo llama `output_common.c:129`; si Lua arranca antes, cargar
   el blob en una función propia que asegure `pvr_init` primero.
4. **Lectura**: `luaL_loadbufferx` lee el puntero directamente. Las lecturas
   de VRAM desde la CPU no pasan por caché y son lentas, pero solo ocurren al
   cargar un módulo (arranque, primer menú). Si el arranque o la apertura
   del menú se nota más lenta (> 50 ms), cambiar a `lua_load` con un lector
   que copie 4 KB de VRAM a RAM por llamada.
5. **Vida**: el bloque se pide una vez, antes que las páginas del primer
   nivel, y no se libera nunca (no fragmenta la VRAM entre niveles).
6. **Fallo**: sin VRAM o sin archivo → se queda con el blob en RAM (malloc)
   y un `LOG_WARNING`; si tampoco cabe, error fatal claro al arrancar.

**Medir**: tamaño del ELF (`.rodata`) y heap libre tras cargar Venice, antes y
después (debería ganar ~300 KB); tiempo de arranque y de la primera apertura
del menú de ajustes; que todos los módulos perezosos carguen (abrir cada
pantalla del menú, inventario, pasaporte). **Criterio**: +~300 KB de RAM
libre, sin errores de Lua, aperturas ≤ 50 ms más lentas.

**Riesgos**: lecturas de 8 bits de VRAM en la zona de 32 bits (deberían ir;
probar en consola real además de Flycast); un módulo pedido mientras el PVR
renderiza compite por el bus (solo una pausa breve).

---

## Al terminar cada una

Fila en PROBADO (OK/FALLIDO, cifra, DC build N), detalle en REGISTRO, quitar
la fila de "mipmaps del mundo" de TAREAS (sección E, 21) cuando V2 cierre, y
retirar sondas `TEMP`.
