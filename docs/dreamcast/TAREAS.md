# Tareas a realizar

Encargos listos para que los tome otra sesión o agente. Cada uno se entiende
sin la conversación en que surgió. Antes de empezar, lee
[TRASPASO.md](TRASPASO.md) (entorno, ciclo de compilación, reglas con
Flycast) y [REGISTRO.md](REGISTRO.md) (lo ya hecho y medido). La lista
general de pendientes sigue en TRASPASO.md §7.

Al terminar una tarea: anótala en REGISTRO.md con build y cifras, y bórrala
de aquí.

---

## 1. Perfilar y optimizar la lógica de juego y la preparación de la escena

**Estado**: por hacer · **Origen**: sesión del 21/10/2026, tras dejar el
batcher en 17,0 ms.

### Contexto

En Venecia (`venice.tr2`, nivel 3 del selector), DC build 200, Lara quieta al
empezar, 30 fps estables en Flycast. Reparto de un fotograma (línea
`[frame]`, en 0,1 ms):

| Parte | ms | Qué es |
|---|---|---|
| `logic` | ~3,9 | Desde el final de un fotograma al inicio del siguiente, menos el tiempo dormido: lógica de TR (ítems, Lara, colisiones, cámara), eventos y scripts Lua, interpolación |
| `stage` | ~5,0 | Dentro del dibujo, todo lo que no son las pasadas del batcher: recorrer salas por portales, decidir objetos visibles, matrices de Lara y objetos (`Matrix_*`, `Output_CheckBoundsClip`), preparar instancias (`M_Stage`, `MeshBatcher_Stage`), sprites, HUD/UI |
| `pass` | ~17,0 | El batcher (ya trabajado a fondo; ver REGISTRO.md) |

Margen actual: ~5,7 ms sobre 33,3. El objetivo es ganar margen para zonas más
cargadas y para el audio que vendrá, sin cambiar el aspecto ni el
comportamiento del juego.

### Qué hacer

1. **Medir antes de tocar.**
   - Perfilador por muestreo: `meson configure build/dc-minsize
     -Ddc_profile=true`, compilar, disco de prueba de Venecia
     (`make_disc.sh ... build/dc-minsize/TRX-prof.cdi 3`),
     `run_flycast.ps1 -Seconds 75`, y
     `py -3 -I tools\dreamcast\prof_report.py <log> build\dc-minsize\TRX.elf
     G:\DreamSDK\opt\toolchains\dc\sh-elf\bin\sh-elf-addr2line.exe 4`.
     **Volver a `-Ddc_profile=false` al acabar.**
   - El perfilador aliasa con los 30 fps; para repartos fiables, poner
     temporizadores temporales (`timer_us_gettime64`) alrededor de los
     bloques grandes y sacarlos con `printf` junto a la línea `[frame]` de
     `M_StatsEndScene` (`render/output_common.c`). Marcar ese código para
     retirarlo y **retirarlo** antes de dar la tarea por hecha.
   - Para saber quién llama a una función de librería (`memcpy`,
     `__floatdisf`, `__divdi3`, `mutex_unlock`...), el truco que funcionó:
     en `M_Profile` (`dc_watchdog.c`) contar el PR solo cuando el PC cae
     dentro de esa función (direcciones con `sh-elf-nm -n TRX.elf`), y leer
     con el modo `pr` de `prof_report.py`.
2. **Partir `logic`**: tiempo de `Game_Control`/fases, ítems activos, Lara,
   cámara, efectos, Lua (`luaV_execute`, `M_FireEvent`,
   `M_CompactListeners`), interpolación. En perfiles anteriores salían
   `Object_Get`, `CatalogTable_TryGet` y `Catalog_IsValidID`: buscar llamadas
   repetidas por frame que se puedan recordar (como se hizo con el traje de
   Lara en `lara/skin/common.c`).
3. **Partir `stage`**: recorrido de salas (`rooms/draw.c`), dibujo de ítems y
   Lara (`lara/draw.c`, `objects/draw.c`), `M_Stage` en
   `output/sources/objects.c` (arma un `MESH_INSTANCE` de ~400 B por malla),
   sprites, `Output_*` de la UI.
4. **Optimizar lo que pese**, con estos patrones que ya dieron resultado:
   - Divisiones y conversiones de 64 bits: el SH-4 no las tiene (libgcc tarda
     ~100 ciclos). Las matrices de TR son `int64_t`; usar
     `Output_MatrixToFloat` (`output/utils.h`) o float.
   - Copias de estructuras grandes por frame (`memcpy` de instancias,
     matrices): evitarlas o reducirlas.
   - Búsquedas repetidas en catálogos o tablas hash por frame: recordar el
     resultado e invalidarlo donde cambie (carga de nivel, etc.).
   - Lua por frame: ver qué eventos/listeners se disparan cada tick.
5. **Comprobar**: compilar (`build.sh minsize`), disco de prueba de Venecia y
   de La Muralla (nivel 1), comparar `logic` y `stage` antes/después, y una
   captura (`flycast_shot.ps1`, tras `flycast_keys.ps1 -FocusOnly`) para ver
   que Lara y la escena se ven igual. Probar también caminando un poco si se
   tocan cosas de Lara o la cámara.

### Reglas

- Cambios en código compartido, protegidos con `#if defined(__DREAMCAST__)`
  salvo que sean equivalentes exactos en PC (y decirlo).
- Nada de `build/`, CDIs, `textures.vq` ni datos del juego en git.
- No tocar un Flycast que el usuario tenga abierto (`Get-Process flycast`).
- Decir siempre "DC build N" de cada disco.
- Lo que no gane nada medible, deshacerlo y anotarlo en REGISTRO.md como
  descartado.

### Hecho cuando

`logic` + `stage` en Venecia bajan de forma medible (o queda documentado por
qué no se puede), sin cambios visibles, con los temporizadores temporales
retirados, las cifras en REGISTRO.md y un commit en la rama `dreamcast`
(prefijo `dreamcast: `).
