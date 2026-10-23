# Registro de tareas realizadas

Cronológico, lo más reciente al final. Tiempos de `[frame]` en Flycast, con
Lara quieta al empezar el nivel, salvo que se diga otra cosa. "Venecia" es
`venice.tr2` (nivel 3 del selector); "La Muralla" es `wall.tr2` (nivel 1).
Los logs serie de cada prueba están archivados en local en
`build/logs/2026-10-21/` (no entran en git, ver [CLAUDE.md](../../CLAUDE.md)).

## 21 de octubre de 2026 (DC builds 160–202)

### Texturas comprimidas en VQ (builds 160–170)

- **Qué**: las páginas de textura se comprimen al formato VQ del PowerVR al
  hacer el disco (`tools/dreamcast/texvq.c`, llamado por `make_disc.sh`) y se
  cargan ya comprimidas, página a página, sin pasar por un bloque de 16 bits.
  El juego encuentra cada página por la huella FNV-1a de su contenido
  (`textures.vq`, índice `{hash, posición}` ordenado por hash, datos en el
  orden en que aparecen las páginas).
- **Quedan en 16 bits**: las páginas ARGB4444 (fuentes de bordes suaves) y
  las que retocan las inyecciones (1–2 por nivel).
- **Resultado**: VRAM libre tras cargar Venecia 1121 → 4420 KiB; desaparece el
  pico de 4 MB durante la carga. La Muralla: 25 de 26 páginas en VQ.
- **Carga**: el primer intento la alargó ~1,7 s; tras huella por palabras,
  lectura en orden, transparencias por bloques de 2×2 dentro de `pvr.c` y
  muestreo 1 de cada 4 en el chequeo de gris de las pistolas, Venecia carga en
  12,1 s (de ellos 2,4 s son la carga de sonido, ajena a esto) frente a
  9,6 s sin sonido antes.
- **Efecto secundario**: cada mano de Lara tiene una cara menos con reflejo
  (13/17 en vez de 14/17) por el muestreo del chequeo de gris.

### Niebla y distancia de dibujo (build 161)

- Niebla negra fija en todos los modos: empieza a 12 casillas, a 20 no se
  dibuja nada (`Output_ApplyLevelSettings` en `render/output_common.c`). Se
  quitó el límite por modo de `dc_mode.c`.
- Los niveles de TR2 no definen color de niebla; el de la configuración ya
  era negro, igual que el fondo.
- Consecuencia: Venecia pasó de 2358 a 5548 vértices y de 483 a 1174
  polígonos en el mismo sitio, y de 31 a 27 fps.

### Rendimiento de Venecia: de 27 a 30 fps estables (builds 171–200)

| Build | Cambio | Efecto |
|---|---|---|
| 172 | Luces de sala descartadas por instancia (esfera de la instancia contra el radio de la luz); `fsrra` para 1/z y la distancia de niebla | xform 12,0 → 9,3 ms; 29 fps |
| 174 | Color guardado de las salas atenuado con enteros bajo niebla negra, sin reiluminar | xform 8,0 ms |
| 177 | Espera al PowerVR movida del inicio al final del fotograma (`m_ScreenPending`): la CPU construye mientras se dibuja el anterior | wait 2,3 → 0 ms; 30–31 fps |
| 177 | Bucle rápido de vértices para salas (prefetch, `fipr`) | xform 7,3 ms |
| 179 | `Output_CheckBoundsClip`: división en float en lugar de 16 divisiones de 64 bits por objeto | stage 5,8 → 5,6 ms |
| 181 | UV de la cara solo tras descartar las de espaldas; `M_Inset`; `M_Project` con una comprobación para el caso normal | pass 20,4 → 19,6 ms |
| 185 | `MeshBatcher_Stage` copia la instancia (~400 B) solo en las pasadas que dibuja; `Output_MatrixToFloat` (int64 → float con dos conversiones nativas) | stage 5,2 ms, pass 19,4 ms |
| 186 | Caché de cabeceras PVR (64 huecos, se vacía al cambiar cualquier textura) | pass 18,7 ms |
| 188 | Traje de Lara recordado hasta que cambia (antes, búsqueda en el catálogo por malla y frame); recorte de colores en enteros | stage 5,0 ms, pass 18,6 ms |
| 191–200 | Marca de estado cambiado en `M_Reserve`; polígonos proyectados directo al búfer de la lista (`DC_PVR_ReservePoly`); bucle propio para mallas iluminadas por la luz del objeto (`plain_object`, un `fipr` compartido con el cel) | xform 6,0 ms, pass 17,0 ms |

Estado en la build 200 (Venecia): logic 3,9 · stage 5,0 · pass 17,0 · map
1,4 · submit 0,3 ms → ~27,6 ms de 33,3, **~5,7 ms de margen**. La Muralla:
pass 10,3 ms.

Reparto del batcher medido con temporizadores temporales (build 190):
transformación 7,2 · caras opacas 3,8 · caras punch-through 1,7 · pase cel
2,6 · sombras suaves 1,0 · relieve 1,0 · transparentes 0,8 ms. El 71 % de los
vértices transformados cae en pantalla; el 34 % de los quads está entero
fuera.

### Probado y descartado (sin ganancia en Flycast)

- `-O2` para `batcher.c` y `pvr.c` (librería estática con
  `override_options`): xform algo peor, resto algo mejor, neto nulo.
- `DC_PVR_Depth3D` en línea.
- Fusionar el pase cel con la emisión opaca (builds 201–202): 17,0 → 17,0 ms.

### Herramientas de medida usadas

- Perfilador `dc_profile` + `prof_report.py`; modo temporal que cuenta el
  llamador solo cuando la muestra cae en `memcpy`/`memset`/`__floatdisf`
  (así se encontraron las copias de instancias y las conversiones de 64 bits).
- Temporizadores temporales por fuente del compositor y por parte de
  `M_DrawInstance` (ya retirados).

## TR3 en marcha (21/10/2026, DC builds 215–216)

Primer arranque de TR3 (Jungle, `TRX_DC_GAME=tr3`): carga y corre a 30 fps
(~850 polígonos, heap 12,3 MB casi lleno). Lara salía hecha solo de muslos con
pistolera: sin `lara_outfits.bin`, `Lara_Skin_Initialise` copiaba las mallas
de `O_LARA` (en TR3, comodines) antes de que `Lara_Mesh_Initialise` lo apunte
a `O_LARA_SKIN`. Ahora el camino sin traje toma `O_LARA_SKIN` si está cargado
(build 216: Lara completa). Revisado sin cambiar: niebla negra, como el TR3
original. Pendiente en TAREAS.md §4: luz TR3, partículas, una sombra.

## TR3: luz de color, chispas y una sombra (21/10/2026, DC builds 217–218)

- **Una sombra**: `DC_Mode_GetShadowLights` (dc_mode.c) da 1 luz en TR3, la
  del modo en TR2.
- **Luz de objetos TR3** (batcher.c): `M_PrepareInstanceLight` lee el bloque
  como `OUTPUT_UNIFORM_LS_TR3` (ambiente + 3 luces RGB, direcciones al
  espacio del objeto); `M_TR3ObjectRGB` por vértice en `M_XformObject` y
  `M_LightVertex`, doblado por la curva (`tr3_double`, 1 en "flat"). La luz
  más fuerte guía el cel. Luz propia: ambiente × shade8 como
  `lightOwnTR3`.
- **Luz dinámica TR3**: las luces `OUTPUT_DYNAMIC_LIGHT_RGB` (antes
  descartadas) suman color en salas (`M_XformRoom`, solo si alguna toca la
  sala) y en el camino genérico (`M_TR3DynamicRGB`).
- **Chispas**: `OutputSource_PolyFX_StageSpark` deja de ser stub: en
  sprites.c (bloque DC) cada chispa es un cuadrado en billboard escalado a su
  tamaño, con textura de sprite o plano, tinte = su color; mallas creadas al
  primer uso y liberadas con las de los sprites. Humo de pistola visible.
- Jungle, Flycast: 30 fps; stage ~6, pass ~12 ms (con armas fuera). TR2 sin
  cambios de camino (todo bajo `il->tr3` o `g_TRVersion`), no re-medido.

## Memoria: salas compartidas, sectores estrechos (21/10/2026, DC builds 220–222)

Jungle tenía 2,4 MB más que Great Wall: 165 salas contra 84 (56.000 esquinas
de sala contra 24.000) y 11.854 sectores contra 6.556. La caché de mallas de
objetos era pequeña (el `meshes=` del watchdog cuenta sobre todo salas).

- **Salas compartidas** (batcher.c, `M_ShareVertices`): al registrar una
  malla de sala, cada vértice distinto se guarda una vez; cada esquina guarda
  su índice (`bind->vref`, 2 B) y su textura (`bind->uvw`, 2 B) en vez de 28
  B. Lectores vía `M_Corner`. Solo salas: los objetos reescriben sus esquinas
  al moverse (objects.c, `M_ForEachFaceVertex`). Jungle −813K.
- **SECTOR estrecho en DC** (rooms/types.h): enums en un byte, splits en
  int16/int8 (15 clicks máx.), campos sin relleno: 100 → 56 bytes, SURFACE
  32 → 16. Jungle −488K; Great Wall en total −618K.
- **Tope de la caché de objetos**: `OutputSource_Objects_EvictToBudget`
  (24.000 vértices, cada 30 frames, nunca las del último frame). No actúa en
  Jungle ni Great Wall; red de seguridad.
- Resultado: Jungle 12.368K → 11.067K usados (brk 0x8cdf2000, ~2 MB al
  límite); Great Wall 9.988K → 9.370K. 30 fps, Lara pisa bien las rampas.
- Descartado: liberar vértices/caras crudos de sala (shadows.c los lee en
  juego); `-fshort-enums` (la config escribe enums por `int32_t *`).

## Coleta de Lara en TR2 y TR3 (21/10/2026, DC builds 225–232)

Sin `lara_outfits.bin` no había coleta: TRX la saca del objeto extra del
traje. Ahora, sin traje, se usa la del propio nivel, como los juegos
originales: el objeto `O_LARA_HAIR` (id 2 en TR2 y TR3), una trenza desde su
malla 0, colgada donde dice el traje clásico del juego (`tr2_classic` /
`tr3_classic`, buscado una vez por juego). En lara/skin/common.c:
`M_UsesLevelBraid`, `M_GetBraidSetupOutfit`; vestida no cambia nada. Los
modos TR1 (cambian cabeza y torso) quedan fuera.
Verificado: Great Wall cuelga por la espalda; Jungle ondea con el viento.
Memoria ~+30K, 30 fps. CDIs: TRX.cdi = build 231, TRX-tr3.cdi = build 232.

## Salas: un vértice compartido se transforma una vez (21/10/2026, DC builds 233–236)

Desde el reparto de vértices de sala (`M_ShareVertices`) la memoria era
compartida, pero `M_XformRoom` seguía transformando, iluminando y
nieblinizando cada esquina. Ahora cada vértice único se hace en la primera
esquina que lo pide en la instancia y las demás copian su `M_XVERT`
(`batcher->share_slot`: generación << 16 | esquina, ~4 B por vértice único
de la sala más grande). Vale con los trozos de sala recortados.
Sondas nuevas (TEMP, `[probe]`): `troom`, `tplain`, `tgen` = tiempo en cada
ruta de `M_Xform*`.
Medido oculto y sin teclas: Great Wall salas 1,81 → 0,95–1,04 ms, sgflush
8,4–9,1 → 7,4–8,0 ms; Jungle (715 polígonos) pass 14,3–14,6 → 13,1–13,3 ms.
Sin comprobar a la vista (escritorio oculto, sin capturas).
Pendiente aparte: la ruta genérica cuesta 3,2 µs/vértice frente a 0,9–1,1
de las otras (360 vértices, 1,16 ms en Great Wall).

## Objetos: esquinas gemelas y reflejos por la ruta rápida (21/10/2026, DC builds 237–241)

- **Gemelas** (`M_FindTwins`, batcher.c): un objeto guarda un vértice por
  esquina (objects.c los reescribe), pero las esquinas de un mismo vértice
  dan lo mismo. Al añadir la malla, cada esquina apunta a la primera igual
  (`bind->twin`, 2 B por esquina, solo si hay alguna); `M_XformObject` la
  copia en vez de calcularla. Si la malla se reforma
  (`MeshBatcher_UpdateMeshGeometry`: uniones, flags) el mapa se suelta y no
  se vuelve a hacer (`bind->deforms`).
- **Reflejos en la ruta de objetos**: `M_EnvVertex` sale de
  `M_XformGeneric` y la usan las dos; las mallas con reflejos (pistolas y
  muslos de Lara) ya no caen en la genérica. Atajo: el color iluminado se
  recupera del ya nieblinizado (anotado en el código).
- Sondas TEMP nuevas: `gown genv gfog gbill goth` = vértices de la ruta
  genérica por motivo (en Great Wall eran todos reflejos).
- Medido oculto: Great Wall objetos 2,21 → 1,03 ms, sgflush 8,4 → 7,2 ms;
  Jungle objetos 4,14 → 1,71 ms, sgflush 12,3 → 9,9 ms, pass 13,1 → 10,5
  ms. Memoria +4–5K. Sin comprobar a la vista.
- Probado y descartado: `movca.l` al escribir cada vértice en la lista
  (evita leer la línea de caché). Flycast no emula la caché: no se puede
  medir aquí; candidato para hardware real.

## sgflush y sui en TR2: lo que se midió sin ganancia (21/10/2026, DC builds 242–249)

Todo oculto en Great Wall; nada de esto queda en el código salvo las sondas.
- **Emit** (2,8 ms): contadores TEMP `equad etri eclip eout eback` por
  frame: 172 quads y 165 triángulos emitidos, 93 recortados en el plano
  cercano, 595 fuera de un lado y 441 de espaldas. Lo descartado es barato;
  lo emitido, trabajo necesario. Sin despilfarro claro.
- **Bump** (0,8 ms): descartar las caras de espaldas antes de su luz no
  cambia nada (dentro de una sala casi ninguna lo está).
- **sui** (1,4 ms): casi todo es Lua: `ui_reserve_all` (e48) ~0,84 ms y
  `ui_paint` (e27) ~0,31 ms. Las medidas de widget salen de la caché (0,04
  por frame, 26 widgets); el coste está en leer las visibilidades: señales
  derivadas sin oyentes, que se recalculan en cada lectura porque `tick`
  cambia la época global cada frame. Probado sin ganancia: que el widget
  escuche su `visible` (peor, 0,39 → 0,84 ms), `derive` sin tabla para 1–2
  fuentes, GC generacional (Lua 2,17 → 2,05 ms, picos 4,7 → 2,9 ms; no
  adoptado sin probar Venecia).
- **movca.l**: no medible en Flycast (no emula la caché).

## Entrada y sombra suave en TR2 (21/10/2026, DC builds 251–253)

- **Entrada**: en DC, `Input_IsBackendEnabled` (input/common.c) apaga el
  teclado y el táctil: sdl_compat.c nunca genera sus eventos, y cada tick
  se preguntaba por los ~60 roles en ellos para nada. `inp` 0,50 → 0,17
  ms; logic 2,4 → 2,1 ms.
- **Sombra suave** (`map`, 1,3 ms, que también va dentro de `sscene`):
  sondas TEMP `msil` (siluetas) y `mend` (cerrar el frame del mapa): 1,22 y
  0,12 ms. Las siluetas mandan los quads como tira de cuatro
  (`M_SilFront`, `M_SilVertex`) y las esquinas gemelas copian su
  proyección (`bind->twin`): 1,22 → 1,10 ms. El resto es un triángulo por
  celda (la de la luz y las de la auto-sombra).
- Mirado sin cambio: `lwater` (0,41 ms) es sobre todo la interpolación de
  la matriz entre fotogramas de la animación, que ya va en floats en DC.
- Total TR2 en Great Wall: logic 2,1 + stage 3,6 + pass 7,2 + map 1,2 ≈
  14,3 ms por frame (antes ~15).

## Partida cargada sin pistolas ni coleta (21/10/2026, DC build 255)

Sin `lara_outfits.bin` Lara va sin traje (`m_SkinType = M_NO_OUTFIT`). Al
guardar, `skin_is_default` sale falso; al cargar, `Lara_Skin_SetType` no
encontraba ese tipo y la vestía con el de reserva: desde entonces se creía
vestida, y la coleta y las pistolas de las fundas se buscaban en mallas de
traje que el nivel no tiene. Ahora `Lara_Skin_SetType` no hace nada en un
nivel que no puede vestirla (`M_CanDress`), como ya hacía
`Lara_Skin_Initialise`. Las partidas guardadas no cambian.
Posible misma causa: el crash al abrir el inventario tras cargar (build 250,
`MeshBuilder_AddVertices: vertex_count > 0`). Queda un diagnóstico TEMP en
`M_BuildMesh` (objects.c) que nombra la malla y el objeto si vuelve.
Partidas en la VMU: 14–15 bloques cada una.

## Sombra suave de TR2: 2 luces, más oscura cerca, sin estirarse en paredes (21/10/2026, DC builds 256–257)

- `shadow_lights` 3 → 2 en Gráficos y Rendimiento (dc_mode.c).
- Intensidad: el peso de cada luz (`M_ConsiderLight`) es 1 a un bloque o
  menos de la luz (`M_SHADOW_NEAR`, o media distancia de alcance si es
  corta) y baja a 0 al final de su alcance; antes era lineal y solo daba
  0,95 pegada a la fuente. Mínimo y máximo, los de siempre (0,5–0,95; sin
  luz cerca, ~0,63).
- Paredes: con la luz encima, una pared es paralela a la luz y la silueta
  se alargaba por toda su altura. Cada cara del mundo recibe la sombra por
  el coseno entre ella y la luz: nada por debajo de 0,25, entera desde 0,6
  (`M_SOFT_GRAZE_MIN/FULL`, en `M_EmitShadowFaces`).
- Coste, mismo punto de Great Wall: suave map 1,2 + pass 7,3 ≈ 8,5 ms;
  volúmenes map 0 + pass 8,3 ≈ 8,3 ms (construirlos sube sgflush 1 ms).
  Los volúmenes cargan además al PowerVR (32 KB de lista de volúmenes), y
  eso Flycast no lo mide bien.
- Agua: en la poza de Great Wall la sombra caía en el fondo, bajo el agua,
  y a través de ella parecía estar en la superficie. Las salas bajo el agua
  (`room->flags.underwater`) ya no reciben la sombra de Lara, ni suave ni
  de volúmenes (`M_DrawInstance`).
- Sin comprobar a la vista. Pendiente: vidrio rompible (sitio por ver).

## Rendimiento en TR3: Lua de la interfaz, ruta de luz propia, luz por instancia (22/10/2026, sin build de disco)

Origen: log de TR3 en Flycast (escenas de 1100–1400 polígonos, 23–31 fps):
`sui` 3–4 ms (`UI_PAINT` 1,6–2,6, `UI_RESERVE_ALL` 1,1), `tgen` 2,2–2,9 ms para
~1000 vértices (2,35 µs cada uno, el triple que las otras rutas), `emit`
5,8–6,7 ms. **Compilado, sin probar ni medir.**

- **Señales** (`src/lua/trx/signal.lua`): una señal derivada sin oyentes se
  recalculaba entera en cada lectura tras cualquier cambio en cualquier señal,
  y `tick` cambia cada frame. Ahora cada señal lleva una versión (`_ver`,
  sube al cambiar) y la derivada recuerda las de sus fuentes (`_seen`): si
  ninguna cambió, devuelve el valor guardado sin llamar a la función.
  Atajo: una función derivada que lea estado ajeno a sus fuentes ya no se
  refresca cada frame (las del juego leen solo sus fuentes).
  Queda sin tocar `TICK` (1,2 ms, oyentes de señales sondeadas).
- **`M_XformOwn`** (batcher.c): luz propia, billboards y mallas sin luz, en
  enteros, para el caso con niebla negra, sin luz de sala/objeto ni
  reflejos. Cubre `gown`, `gbill` y `goth` de las sondas; TR1/2 y TR3.
  Esperado: `tgen` 2,4 → ~1 ms. Comparar `tgen` con la línea `[probe]`.
- **Luz por instancia**: `M_ReadInstanceLight` lee `inst->light_info` directo
  (sin memcmp/copia/subida al búfer GL ni `1/sqrtf` por dirección) en TR1–3;
  TR4 sigue por el camino de antes. Las luces de paso de las salas se suben
  una vez por modo de luz y pasada, no por instancia.
- **Trozos de sala**: `M_MAX_CHUNKS` 64 → 128 (celdas más finas en salas
  grandes; se recorta más `xform`/`emit` fuera de pantalla).
- **No hecho — descarte de caras de espaldas por plano**: ahorra lecturas de
  `M_XVERT` (caché), que Flycast no emula, así que no se vería aquí, y
  costaría ~8–16 B por cara de sala. Reabrir solo con hardware real.

## Atajo de fin de nivel y efectos de sonido (22/10/2026, sin build de disco)

- **L + R + X + Y** a la vez terminan el nivel, como el baile del truco
  (`Lara_Cheat_CheckKeys`, misma opción `flow.cheat_keys`). El mando lo
  detecta al pulsarse (`DC_Input_TakeEndLevelCombo`, sdl_compat.c).
- **Efectos de sonido** (`src/platform/dreamcast/audio.c`): cada muestra WAV
  pasa a ADPCM Yamaha de 4 bits en la RAM del AICA (un cuarto de su tamaño
  en 16 bits); las de más de 65534 muestras bajan a la mitad de frecuencia
  hasta caber. 50 canales propios (`snd_sfx_chn_alloc`), uno por sonido.
  Volumen y pan con la fórmula del mezclador de PC, cambiados en vivo con
  `AICA_CH_CMD_UPDATE`. Atajos: una pausa vuelve a empezar los bucles y
  deja caer los cortos; sin reverb. La música (streams) sigue en stub.
- **Compilado, sin probar.** Mirar en el log el ERR `Unexpected sample
  header` (ya existía) y si alguna muestra no cabe en la RAM de sonido.

## Música, pausa de efectos y MAIN.SFX del remaster (22/10/2026, DC builds 269–275)

- **MAIN.SFX**: el del remaster (TR2 y TR3) empieza con una tabla de 740
  bytes antes de la primera WAV; el lector compartido abortaba en la
  primera muestra y no cargaba ningún efecto (el viejo ERR `Unexpected
  sample header`). `M_InitialiseSamplesFromFile` salta hasta el primer
  RIFF en DC. Carga: ~1,7 s (paso a ADPCM incluido).
- **MS-ADPCM** (formato 2, la inyección `lara_rifle_sfx.bin`) se decodifica
  en `audio.c`.
- **Pausa**: las muestras van a la RAM de sonido por `snd_mem_malloc` y los
  canales se arrancan a mano (`AICA_CH_CMD_START`), así que una pausa
  guarda la posición (`snd_get_pos`) y sigue desde ahí; los bucles vuelven
  a empezar.
- **Música**: `make_disc.sh` convierte `TRACKS/*.OGG` a WAV ADPCM Yamaha
  estéreo 44,1 kHz (`build/<dir>/music-<juego>`, ~2,6 MB/min; TR2 43
  pistas, TR3 73; discos de 264 y 362 MB). `audio.c` las reproduce con los
  streams de KOS desde un hilo (`trx-music`, 20 ms), con fundidos, bucle,
  inicio/fin y callback de fin. Atajos: sin filtro paso bajo bajo el agua
  y sin cambio de velocidad.
- Probado en Flycast (sin oír): el nivel 1 de TR3 carga todas las
  muestras, `Playing track 34` sin errores, 30 fps. El arreglo de señales
  bajó `UI_PAINT` de ~2,5 a 0,33 ms y `M_XformOwn` deja `tgen` en 0,38 ms.
- **Reverb**: no hecha; necesita un programa propio para el DSP del AICA.

## Música que no sonaba y atajo en casa de Lara (22/10/2026, DC builds 276–284)

- **Música muda** (build 274): KallistiOS arranca los dos canales de un
  stream a la vez con una máscara de 32 bits (`cmd_id`), y los streams
  caían en los canales 50–51 porque los 50 de efectos se reservaban antes:
  nunca arrancaban. Ahora `Audio_Init` deja libres los canales 0–7 para la
  música (efectos en 8–57).
- **Ritmo**: cada sondeo pide medio búfer por canal ya intercalado (32704
  bytes); el búfer intermedio daba la mitad y la lectura iba 5 veces más
  rápida que el sonido. Ahora 32 K; medido ~43 KB/s (44,1 kHz estéreo).
- Pila del hilo `trx-music` 8 → 32 K (las lecturas del disco bajo
  `snd_stream_poll`); la build 274 se cerró con "SH4 exception when
  blocked" en Lara's Home; 100 s seguidos en la 282 sin fallo.
- **Atajo L+R+X+Y**: no se miraba en Lara's Home (el gym salta todo el
  truco de las teclas). Ahora se mira antes. En el teclado de Flycast por
  defecto: F + V + S + D.

## Sonido sin retraso, cinemáticas, demos y benchmark (22/10/2026, DC builds 285–292)

- **Heap del juego**: lo que no es del nivel toma el bloque libre más alto de
  todos los cajones (no solo del primero que cabe); en la 283 el mayor hueco
  bajaba de 6,7 a 1,6 MB en cuatro cargas y Jungle abortaba. Ahora 8 cargas
  de partida seguidas (TR3, '1 8'): el mayor hueco se queda en 5,2–6,5 MB.
- **Sonido**: el AICA deja el canal en key-on al acabar una muestra sin
  bucle, así que `snd_is_playing` decía que todo seguía sonando: los huecos
  se agotaban (pisadas que faltaban) y el juego mandaba cada frame volumen
  y pan a sonidos muertos (la cola del AICA se atascaba: retraso). Ahora
  cada sonido lleva su hora de fin y solo se envían los cambios.
- **Cinemáticas**: el hilo que guarda pulsaciones entre lecturas seguía
  sumando durante la carga lo que estaba apretado (X e Y del atajo), y la
  primera lectura de la cinemática lo veía como pulsado: se saltaba en el
  cuadro 0. Lo guardado se tira si la lectura anterior tiene más de 250 ms.
- **Demos** al final del selector (TR2: Venice, Maria Doria, Tibetan
  Foothills; TR3: Aldwych, River Ganges, Nevada) y, en TR3, **Benchmark
  (London demo)**: la demo de Aldwych medida entera (`dc_bench.h`,
  `output_common.c`), resultado 10 s en pantalla y en el log:
  `[bench] 2487 frames avg 26.3 fps min 5.1 slow 50% | logic 5.0 stage 9.6
  pass 15.2 map 1.2 pvr 7.5 ms` (build 290, Flycast).
- Atajo de fin de nivel en el teclado de Flycast: F + V + S + D.

## Selector por páginas e informes de benchmark (22/10/2026, DC builds 293–294, sin probar)

- Selector de niveles en tres páginas (derecha/izquierda): niveles,
  cinemáticas en el orden de la historia (TR2 4, TR3 11; `GF_START_CINE`
  vía `g_DC_StartCutscene`) y demos con benchmarks (TR3: Londres/Aldwych;
  TR2: Venice).
- Informe detallado del benchmark por serie, líneas `[bench] ` entre
  `=== report` y `=== end`: fecha, build, juego, demo, modo, fps
  medio/mínimo, mediana y 99 % de la duración del frame, frames lentos,
  cada stat (media/peor), todas las sondas e histograma por ms.
  `tools/dreamcast/bench_archive.ps1` los guarda en
  `docs/dreamcast/benchmarks/<fecha>_build<N>_tr<n>.txt`; `run_flycast.ps1`
  lo ejecuta al cerrar Flycast (con `-KeepRunning`, a mano sobre el log).

## Nueva medida de Venecia (22/10/2026, disco "DC build 256" del contador local)

- **Ojo con el número**: el disco salió como "DC build 256" porque el
  contador `build/dc-build-number` de esta máquina (D:\trx) va por detrás del
  de este registro; no es la build 256 de la sombra suave. El código es
  `a996c8ba7` más los cambios sin commit del selector por páginas y los
  informes de benchmark (builds 293–294).
- Datos TR2 clásicos de Steam, disco de prueba en el nivel 3, Lara quieta,
  75 s en Flycast oculto; media de 11 ventanas `[frame]` (la primera, con la
  carga, fuera). Log: `build/dc-venice-b256.txt`. 30 fps estables.

| Parte (ms) | Build 200 | `dc-venice-final.txt` (21/10) | Hoy |
|---|---|---|---|
| `logic` | 3,9 | 2,57 | 2,41 |
| `stage` | 5,0 | 5,14 | 5,49 |
| `pass` | 17,0 | 14,55 | 11,64 |
| `map` | 1,4 | 1,10 | 1,20 |
| `submit` | 0,3 | 0,30 | 0,30 |
| **Total** | ~27,6 | ~23,7 | **~21,0** |
| **Margen** (33,3) | ~5,7 | ~9,6 | **~12,3** |

- `pvr` 7,5 ms en paralelo; `wait` 0: la CPU nunca espera al PVR.
- Polígonos 889 por frame (939 en el log del 21/10): parte de la bajada de
  `pass` puede venir de ahí. `stage` es ya la mitad de `pass`.
- 60 fps (16,6 ms) quedan a ~4,4 ms.
- Segunda pasada con sondas nuevas (`src*`, `p*`, `u*` en `dc_probe.h`; disco
  "DC build 259" del contador local, log `build/dc-venice-probes2.txt`):
  `UI_Draw` 0,07 ms (5 operaciones); el batcher es 11,47 de los 11,8 ms del
  pass y `M_DrawInstance` 11,42; caras transparentes 0,70 ms (216 por
  frame). Candidatos y reparto completo en TAREAS.md §5.

## Hacia 16,6 ms: candidatos aplicados (22/10/2026, DC builds 260–262 del contador local)

Venecia, Lara quieta, Flycast oculto, media de 11 ventanas; logs
`build/dc-venice-opt1.txt` (260) y `build/dc-venice-opt2.txt` (261).
**21,0 → 19,7 ms** (logic 2,4 · stage 4,8 · pass 11,6 · map 0,6 · submit 0,3).
La Muralla (build 262, `build/dc-wall-opt2.txt`): logic 2,1 · stage 3,0 ·
pass 7,1 · map 0,6 = 12,8 ms (sin medida previa con el mismo código).
Captura `build/shot-venice-261.png`: el canal, la barca y las paredes se ven
como antes.

| Cambio | Dónde | Medido |
|---|---|---|
| Reservas de regiones de la UI guardadas entre escenas (`root._reserve`, se olvida con `Widget:wake` o al cambiar una señal leída) | `lua/trx/ui/regions.lua`, `widgets/base.lua` (compartido, mismo comportamiento) | `sui` 1,53 → 0,79 ms; Lua 2,3 → 1,6 |
| Mapa de sombra suave cada 2 frames (`M_SOFT_MAP_EVERY`), con las luces y rodajas con que se dibujó | `batcher.c` `DC_Shadow_RenderMaps` | `map` 1,2 → 0,6 ms |
| Trozos de sala también por orientación (`M_FaceSide`, `M_CHUNK_SIDES`): un trozo de espaldas se descarta antes de transformar | `batcher.c` `M_BuildChunks`, `M_CullChunks` | 196 caras/frame descartadas (`eside`), pero `eback` sigue en 979: **las caras de espaldas son de los objetos** (Lara, ítems), no de las salas. Sin ganancia medible |
| Cada trozo de sala se emite nada más transformarlo (`M_EmitChunk`), con sus vértices aún en caché | `batcher.c` `M_TransformInstance` | Sin cambio en Flycast, que no modela la caché; comprobar en consola |
| Caras transparentes en una sola tanda (`bind->transp`) | `batcher.c` | `ptransp` 0,70 → 0,69: el coste es la emisión, no la llamada por cara |
| Coleta: la soldadura de un puente se guarda mientras sus anclajes no se muevan (`m_LastWeld`) | `lara/hair.c` (bajo `__DREAMCAST__`) | 0 soldaduras/frame quieta (`lweld`); `lhair` 0,38 → 0,33 |
| `exp2f` → `M_Exp2` (exponentes enteros sin libm) | `batcher.c` `M_PrepareRoomLights` | dentro del ruido |

No aplicado:

- **Caché de luz por ítem**: medido primero (`ilight`): 0,14 ms por frame en
  total. No compensa.
- **`TICK` en Lua** (0,9 ms): el reparto entre listeners no se conoce; agrupar
  las lecturas de roles era una suposición. Pendiente de medir en Lua.
- **`M_GetBinding` por instancia**: ~50 µs por frame. No compensa.

Para 16,6 ms faltan ~3 ms, y están en `xform` (4,0) y `emit` (4,5): en
Flycast son coste de instrucciones, no de caché. Lo siguiente sería descartar
las caras de espaldas de los objetos por grupos de normales antes de emitir
(~980 caras/frame), o menos vértices.

## `xform` y `emit`: trozos por orientación, probados y descartados (22/10/2026, DC builds 263–266)

Venecia, Lara quieta, Flycast oculto (`build/dc-venice-opt3..6.txt`).

- **Salas por celda y lado** (`M_FaceSide`, 7 lados por celda) y **objetos
  por cono de normales** (26 direcciones, `M_CullChunks` con la prueba
  |d|·cos(φ+θ) ≥ r): con el signo correcto (build 264–265) se descartan 212
  caras de espaldas de Lara y los ítems por frame (`eback` 979 → 767) y 564
  esquinas menos se transforman (`vobj` 2749 → 2185), pero `tplain` no baja
  (1,78 → 1,75) y `emit` sube (4,58 → 4,70): las esquinas ahorradas eran
  casi todas copias de gemelas (baratas), y el coste por trozo (prueba del
  cono, un `M_EmitIndices` y un cambio de estado por trozo y pasada, los
  sellos de gemelas) se come la ganancia. Pass 11,6 → 11,9 ms. **Deshecho**
  (`git checkout` de `batcher.c` y reaplicado lo que sí vale); la versión con
  conos quedó en la sesión, no en el árbol.
- Dato que costó dos builds: el batcher **dibuja** una cara donde
  n · (ojo − a) > 0 con n = (b − a) × (c − a) en el espacio del objeto (la
  deducción desde `M_IsBackFacing` sale con el signo contrario; `proj_y` lo
  invierte). Con el signo al revés (build 263) `polys` bajó 888 → 763.
- Lo que queda en el árbol de esta ronda: emisión de cada trozo de sala nada
  más transformarlo (`M_EmitChunk`; sin efecto en Flycast, que no modela la
  caché), transparentes en una tanda, `M_Exp2`, sombra a mitad de frecuencia.
- Estado final (build 266): logic 2,5 · stage 4,8 · pass 11,6 · map 0,6 ·
  submit 0,3 = **19,7 ms**, `polys` 889 como al principio.
- Reparto del perfilador dentro de `emit` (muestras útiles de la build 257):
  bucle y despacho por cara (`M_EmitIndices` + `M_TryEmitQuad` +
  `M_EmitTriangle` + `M_EmitFront`) 59 %, trabajo por esquina (`M_Colours`,
  `M_GetFaceUVs`, `M_Project`, `M_Depth`, `M_Reserve`) 31 %, descarte de
  espaldas 4 %. No hay un bloque gordo que recortar: lo que queda es
  trabajo por instrucción repartido, o menos caras.

## Ensamblador SH-4: emisión de caras y transform de salas (builds 267–275)

Hecho el 2026-10-22. Venecia, Lara quieta, Flycast oculto.

- Puntos descartados con datos: copiar `M_XFORM` a locales en `M_XformRoom`/`M_XformObject` (código +116 bytes, sin ganancia: troom 1,55 → 1,53) y listas de caras por octante (el experimento de conos ya descartó 212 caras sin ganar tiempo).
- `render/emit_sh4.S` (`dc_emit_run`): emisión de quads/triángulos de una página directo al buffer de la lista, con descarte por lados y de espaldas, color de offset y copia del cel shading de Lara. Vuelve a C en cruces del plano cercano, quads doblados, cambio de página o falta de espacio. Verificación con `-Ddc_emit_verify=true`: 364 000 tandas, 12,2 M de índices, 0 diferencias. Medida: emit 4,6 → 2,9 ms, pass 11,6 → 9,9 ms, frame ~18,1 ms. `pvr.c`: `DC_PVR_BeginRun`/`DC_PVR_EndRun`.
- `render/xform_sh4.S` (`dc_xform_room`): vértices de sala, caso simple (TR1/TR2, sin luz dinámica, `!vary`, perspectiva, vref): ftrv, near/lados, niebla, shade + parpadeo, tabla de blancos. Verificación (mismo flag, mensajes `[xform-verify]`): 1,6 M de vértices, 0 diferencias. Medida: troom 1,69 → 1,34 ms (pass 99 → 97).
- `render/xform_obj_sh4.S` (`dc_xform_object`): vértices de objeto (Lara, enemigos), TR1/TR2, blanco o tintado, gemelos y cel. Verificado con `dc_emit_verify` (0 bad). Medida: tplain 1,72 → 1,48 ms (pass 94 → 92). La ruta tintada de las salas (`tinted`) también está activa: arreglado un `	` convertido en tabulador en la macro `CHANNEL`.
- Copia de un vértice compartido (32 bytes) con `fschg` + 4 `fmov.d` (alineado a 32): troom −0,06 ms, tplain −0,06 ms; el resto de la copia es el acceso a memoria, no el bucle. Evitarla del todo exigiría que `emit` leyera por la ranura del vértice compartido.
- Ruta TR3 en ambos ensambladores (verificada en la Jungla, 0 bad): salas con `tr3_k` y objetos con la luz de objeto por canal (ambiente + hasta 3 luces, `fipr`). TR3 (Flycast): troom 1,29 → 0,94, tplain 1,79 → 1,57, pass 104 → 98 ms. Quedan en C: salas TR3 con luz dinámica RGB y objetos con reflejo.
- Vértices de sala de Venecia por frame: 2378 en total, 1545 son copias de vértices compartidos (la copia de 32 bytes domina el tiempo que queda), 537 con parpadeo, 811 no blancos.

## Demo de Londres en los tres modos, sombra a 30 Hz, estáticos en ensamblador (22/10/2026, DC builds 314–342)

- **Sombra suave a 30 Hz**: `M_SOFT_MAP_EVERY` vuelve a 1 (el mapa se
  redibuja cada frame); a 15 Hz la sombra no gustaba.
- **Benchmark sin teclas**: `TRX_DC_BENCH=<n>` en `make_disc.sh` (`BENCH.TXT`)
  arranca la demo n entera al encender (TR3: 0, Londres) y
  `TRX_DC_MODE=graphics|performance|experimental-60` (`MODE.TXT`) fuerza el
  modo, porque un `TR3X.json5` en el disco no se aplicaba. Con Flycast oculto
  (`-Hidden`, audio por otro dispositivo) se mide sin tocar el PC. Ojo: el
  `-StopOn` mira el log, hay que borrar el del run anterior.
- **Desbordamiento de pila en las sombras de volumen** (modo rendimiento en
  TR3 caía a mitad de la demo, siempre en el mismo punto): `M_BuildVolume`
  puede dar 12 triángulos de tapa más 2 por cada una de las 12 aristas y el
  búfer era de 24. Ahora 36.
- **`dc_xform_own`** (`render/xform_own_sh4.S`): estáticos y sprites
  (`M_XformOwn`), TR1/TR2/TR3, con cel. Basta que coincidan los bits que
  deciden (sin luz, sin niebla, luz propia, billboard): los demás (descarte
  alfa, aditivo…) varían por mesh. La luz coloreada de TR3 sale de una tabla
  de 256 pasos de sombreado cacheada por (ambiente, doblado, tinte).
  Verificado con `dc_emit_verify` (0 bad) en TR2 y en la demo de Londres. El
  verificador admite un nivel de diferencia por canal: el ensamblador suma y
  multiplica los float por separado donde el compilador los funde.
- **Medidas** (Flycast oculto, Londres entera, ms por frame; antes = build 314
  / 322 / 323, después = 340 / 341 / 342):

| Modo | fps antes → después | pass | frames > 35 ms |
|---|---|---|---|
| Graphics (640x480, 30) | 26,9 → 27,2 | 14,3 → 13,5 | 44 % → 45 % |
| Performance (640x480, 30) | 27,5 → 27,7 | 14,0 → 13,3 | 44 % → 45 % |
| Experimental 60 (320x240, 60 adaptativo) | 39,0 → 39,3 | 11,7 → 11,4 | 15 % → 15 % |

  Informes: `docs/dreamcast/benchmarks/` (`20261022201845_build314`,
  `…220213_build322`, `…220457_build323` antes; `…231914_build340`,
  `…232148_build341`, `…232518_build342` después).
- **Dónde está el tiempo** (Londres, graphics): `pass` 13,5 = transformar
  (salas 1,5, objetos 1,6, estáticos y sprites 3,2) + emitir caras 3,0 + el
  resto del batcher; `stage` 8,0; Lua 3 ms por frame (`UI_PAINT` 1–2,5,
  `TICK` 1,0, `UI_RESERVE_ALL` 0,6–1,6). Los estáticos con luz propia son
  ~2400 vértices por frame y cuestan ~1,1 µs cada uno ya en ensamblador
  (unas 200 instrucciones: colocar, recorte, niebla y color); el compilador
  de tiempos de Flycast cuenta una instrucción por ciclo.
