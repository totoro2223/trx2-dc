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
original. Lo que quedó, en PROBADO.md y TAREAS.md.

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
  frame). Resultado de los candidatos, más abajo y en PROBADO.md.

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
- **Lua de la UI** (22–23/10/2026, builds 343–350): un muestreador por línea de
  Lua (hook de instrucciones, con bytecode sin `-s` y un nombre de chunk por
  script; temporal, no queda en el árbol) repartió el Lua de la demo de
  Londres: señales (`signal.lua`) 0,5 ms por frame, widgets base 0,26, barras
  0,13, accesos a módulos (`helpers.lua`) 0,13, regiones 0,06. Hecho: `track`
  sin `table.pack`/`unpack`, `derive` sin tabla de argumentos para 1–3
  fuentes, `report` en línea en `Signal:get`, y la barra de la UI repite los
  quads que dibujó mientras no cambien posición, tamaño, relleno, tema, suavizado
  ni escala (`w._drawn`). Lua 2,96 → 2,62 ms por frame, UI 2,02 → 1,72.
  Medida final, Londres, Flycast oculto (builds 348/349/350):

| Modo | fps (base → ahora) | pass | frames > 35 ms |
|---|---|---|---|
| Graphics | 26,9 → 27,3 | 14,3 → 13,7 | 44 % → 44 % |
| Performance | 27,5 → 27,8 | 14,0 → 13,3 | 44 % → 45 % |
| Experimental 60 | 39,0 → 39,8 | 11,7 → 11,4 | 15 % → 15 % |

  Informes: `docs/dreamcast/benchmarks/*_build348_tr3.txt`, `…349…`, `…350…`.
  Pistas que quedan: `UI_RESERVE_ALL` sigue entre 0,07 y 1,0 ms por llamada
  (alguna raíz de la UI se recalcula casi cada frame: una señal sondeada que
  cambia cada tick, o un `Tween`), el bucle de roles de `input.lua` (una
  llamada a C por rol y tick) y los ~2400 vértices de estáticos con luz propia.

## Otras siete pistas en la demo de Londres (23/10/2026, DC builds 351–374)

Todo con la demo de Londres entera, Flycast oculto, en los tres modos.
Informes: `docs/dreamcast/benchmarks/*_build372/373/374_tr3.txt` (después) y
`…314/322/323…` (antes de todo).

| Cambio | Efecto medido |
|---|---|
| **1. `-O2` en todo** (`build.sh` pone `optimization=2` en `minsize`; antes `-Os`, solo `batcher.c` iba a `-O2`) | texto 2,69 → 2,91 MB; lógica 4,8 → 4,2 ms, stage 7,7 → 7,0; graphics 27,3 → 28,0, experimental 60 39,8 → 41,7 |
| **2. Lua con recolector generacional** (`lua_gc(L, LUA_GCGEN)`; antes incremental con pausa 100) | Lua −0,22 ms por frame, +0,12 MB de heap de Lua (pausa 130 daba lo mismo con más memoria) |
| **3. Tirones** | ver abajo |
| **4. Relieve adaptativo** | 3 pares apagado/encendido por demo (27, 26 y 25 fps → apaga; vuelve con la escena más ligera): no oscila, sin cambios |
| **5. Efectos** (`Sparks_Draw`) | el staging de PolyFX es un stub en la Dreamcast (`render/stubs.c`): cada chispa pasaba por su posición y profundidad en 64 bits para tirarse al final. `Sparks_Draw` vuelve sin hacer nada en la Dreamcast: `sfx` 1,6 → 0,4 ms, stage −1,2, graphics +0,7 fps y frames lentos 44 → 33 % |
| **6. Estáticos fuera de pantalla** | por llamada a sala: 7–15 estáticos vistos, 5–10 descartados, 2–4 dibujados; el descarte (`Output_CheckBoundsClip`, ~10 µs cada uno) cuesta ~0,5 ms por frame y la luz y el dibujo de los visibles ~0,3: una prueba previa con esfera lo recortaría ~0,3 ms (pendiente, riesgo de popping) |
| **7. Reservas por frame** | `String_IsEmpty` compilaba una regex PCRE2 por llamada (la resolución de rutas la llama ~7 veces por frame, 20 KB de reserva cada una): ahora un bucle. 53 → 30 `malloc` por frame, pass −0,4 ms. Quedan 30 por frame desde `Memory_Alloc` (`Catalog_CreateKey`, `Memory_DupStr`, `String_Format`) |

- **Tirones (3)**: el informe lleva ahora la duración de ~96 frames del medio,
  qué gastó cada parte y los frames de 60 ms o más. Los frames de 29/29/39 ms
  en ráfaga de tres son el limitador de frames con pasos de 10 ms (KOS
  duerme en ticks de 10 ms), no trabajo: la CPU gasta ~23 ms de 33. Lo que sí
  son tirones: cuatro frames de 105–150 ms en el `pass` (la creación de una
  página de relieve: 16 384 texels con `asinf`/`atan2f` cada uno) y una
  racha de ~30 frames de 60–70 ms con 13 500 vértices, `submit` de 16 ms
  (la entrega de ~1600 polígonos a la TA, que Flycast modela) y lógica de 11.
  Hecho: la página de relieve usa un polinomio de atan (error < 1 nivel de 255,
  `[bump-verify]` con `dc_emit_verify`); un texel plano no tiene ángulo, la
  librería le daba −π de un par de ceros negativos.

| Modo | fps (base → ahora) | pass | frames > 35 ms |
|---|---|---|---|
| Graphics | 26,9 → 28,7 | 14,3 → 12,7 | 44 % → 34 % |
| Performance | 27,5 → 29,3 | 14,0 → 12,4 | 44 % → 34 % |
| Experimental 60 | 39,0 → 44,4 | 11,7 → 10,8 | 15 % → 5 % |

## UI reutilizada en frames sin tick (23/10/2026, DC build 375)

Londres, experimental 60, Flycast oculto; base build 374 (mismo código sin
esto). Informe `docs/dreamcast/benchmarks/20261023090144_build375_tr3.txt`.

- **UI**: en un frame fraccionario sin tick (`nframes == 0`) los scripts no
  vuelven a pintar; se dibujan otra vez sus operaciones del frame anterior
  (`UI_ClearPhaseDraw`, `UI_SetScriptCapture` en `ui/draw.c`, decisión en
  `phase/executor.c`). Lo que pinta la fase va en otra lista y se rehace cada
  frame; el orden por capa no cambia. Si la fase empieza una escena propia al
  dibujar, se pinta normal. `sui` 1,47 → 1,10 ms, Lua 1,92 → 1,56, fps 44,4 →
  44,7. Sin comprobar a ojo en pantalla.
- **Luz propia de estáticos con niebla por la tabla** (factor × parte de
  niebla en `xform_own_sh4.S`): `tgen` 2,52 → 2,50 ms, casi ningún vértice de
  luz propia cae en la niebla. **Deshecho.**
- **Agua de Lara**: la sonda `lwater` ya no incluye la mezcla de matrices.
  Agua 0,09 ms (no compensa tocarla); la mezcla de matrices de Lara ~0,34 ms
  (candidata: `M_InterpolateFloat` usa `acosf`/`sinf`).
- `stage` podía salir negativo (y dar la vuelta) cuando la CPU espera al PVR
  antes de empezar la escena: ahora queda en 0.

## Mezcla de matrices de Lara (23/10/2026, DC builds 376–378)

Londres, experimental 60, Flycast oculto.

- Sonda nueva `lblend` (`Matrix_Interpolate` de cada malla de Lara en
  `M_DrawLaraMesh`): **0,33 ms por frame**, unos 11 µs por matriz (dos por
  malla, vista y mundo).
- **nlerp hasta ~50° en vez de 5°** (`M_QuatFSlerp`, umbral 0.999 → 0.9,
  build 376): `slara` 1,263 → 1,253 ms, nada. Casi todos los giros entre
  fotogramas son menores de 5°, así que `acosf`/`sinf` casi no se llaman.
  **Deshecho.**
- **Mezclar solo la matriz de mundo y multiplicarla por la vista** (build
  378): en la Dreamcast la matriz "de vista" de Lara no es `g_ViewMatrix` ×
  mundo (su traslación es la del mundo y `g_ViewMatrix` es casi la identidad
  ahí), así que la comprobación fallaba siempre y `lblend` subió a 0,39.
  **Deshecho.** Queda por saber qué relación guardan las dos pilas en la
  Dreamcast; si una es la otra por una matriz fija, la idea vale ~0,16 ms.

## 24/10/2026 (tarde): humo, sombras, armas, tres juegos

- Se quitó el `return` de `Sparks_Draw` en Dreamcast: ninguna chispa se
  dibujaba (fuego, humo, sangre). Después el humo costaba demasiado: el registro
  por el puerto serie era la mayor parte; una instancia por chispa y la vista
  en coma flotante bajaron el resto (T52).
- Sombra suave más larga y con tope de tamaño (T53). Las armas largas se ponen
  en la espalda con el modelo del inventario (T54). Aviso de guardado en la
  interfaz común (T55).
- Las caídas reinician el disco en la lista de niveles (T56). Disco de tres
  juegos con cambio por Arriba y Abajo y expansiones en la página de Nueva
  partida (T57).
- Pruebas ocultas de flujo (TR1 pasó los 8 niveles; TR2 cayó por memoria al
  cargar el 5; TR3 falló la lectura de una inyección al cargar el 3) y de
  guardado: ver PRUEBAS_PENDIENTES.md, T58.

## 24/10/2026 (noche): memoria entre niveles, opciones en la VMU

- La causa de la falta de memoria al pasar de nivel en nivel era una escritura
  en memoria ya liberada (`Walkable_Reset`), no una fuga de reserva: cortaba la
  lista de trozos de la arena de fotogramas. Después, los bloques pequeños que
  sobreviven a un nivel se sacaron de la región (T59, T60). TR1, TR2 y TR3
  recorren todos sus niveles de seguido.
- Las opciones de render se guardan en la VMU al salir del menú principal y se
  cargan al arrancar (T62).
- Un descuido: las pruebas de guardado usaron la tarjeta del emulador que usa
  el usuario (ver el aviso en PRUEBAS_PENDIENTES.md).

## 24/10/2026 (mediodía): recorrido de niveles y guardados en los tres juegos

Flycast oculto, `TRX_DC_LEVELS` (300 fotogramas por nivel) y `TRX_DC_SAVE=90`
(un guardado cada 90 fotogramas, en cinco ranuras, y una carga tras el quinto),
sin películas. DC builds 311 (TR1), 312 (TR2), 313 (TR3); el mismo TRX.elf
(árbol tras el pull de `424d7d9ae`).

| Juego | Niveles | Resultado | Región del nivel (`game heap`) |
|---|---|---|---|
| TR1 | 0–15, los 16 | Todos cargan; 36 guardados a la VMU; sin caída | 6655 K; pico 5930 K (level10c); retiene 47 K en 5 bloques; mayor hueco libre entre 4454 y 5821 K |
| TR2 | 0–12 | **Cae al cargar Barkhang (12)**: la región se llena (pico 6591 K = toda, 85 464 reservas se desvían al montón de newlib) y el guardado siguiente no encuentra memoria: `Memory_Alloc` aserto, `arch: aborting` | 6591 K; Ópera pico 6539 K con 3 desvíos; Venecia 6036 K |
| TR3 | 0–11 | Cargan los 12; Flycast se cierra solo en Lud's Gate (11) sin nada en el registro serie (síntoma conocido). La región se llena desde Temple (2): 47 365 → 309 710 desvíos acumulados | 6527 K; retiene 42 K en 11–13 bloques; mayor hueco **2383–2643 K** en Rapids, Puna, Sewer y Tower (la mitad partida) |

Lo que se vio además:

- **La carga de partida del gancho nunca cargó nada**: `trx.savegame.load(0)` y
  `save(0)` usan ranuras desde 1 (`trx.savegame.SlotNum`); la 0 no existe
  ("no saved game in this slot"), y `save(0)` fallaba sin aviso. Las pruebas de
  guardado anteriores (T58) solo guardaron. Corregido (ranuras 1–5, carga de la 1).
- **La copia a la VMU falla en silencio cuando el montón está seco**:
  `DC_VMU_StoreSave` hace `malloc` del paquete y devuelve `false` sin aviso
  (TR3 Temple, guardados 7 y 11). La partida queda solo en el disco RAM. Ahora
  avisa (`could not be copied to the memory card: no memory`).
- **TR3 sin VQ**: `texvq.c` solo leía las versiones 0x2D (TR2) y 32 (TR1); los
  niveles de TR3 (0xFF080038/0xFF180038) quedaban fuera y `textures.vq` de TR3
  tenía 39 páginas (las inyecciones). En juego: 12–15 de ~30 páginas en VQ,
  2,7–3,3 MB de VRAM libres (TR2: 4,4 MB). Corregido: mismo lector que TR2.
- **Montón de newlib** (fuera de la región): en TR2 al cargar Barkhang, 11 638 K
  en uso: región 6592 K, Lua 2304 K, y **~2,2 MB en bloques pequeños (< 24 K)**
  de larga vida (catálogos, cadenas, configuración, tablas del flujo); `brk`
  a 124 K del final de la RAM (TR1: 356 K).
- La VMU de 128 K se llena con unas 12 partidas (9–26 bloques cada una).

### Censo de la región (builds 314 y 315, `-Ddc_heap_census=true`)

Opción nueva de Meson (`dc_heap_census`) para el `TRX_DC_HEAP_CENSUS` de la
otra sesión, y un volcado más al terminar la carga ("level loaded"), que da la
composición del nivel. `tools/dreamcast/live_census.py` los lee.

- **Quién parte el hueco**: el mapa de solapes de salas
  (`Room_InitialiseOverlapMap`, 19–165 bytes): se libera y se vuelve a reservar
  al **final** de cada carga, con el nivel ya debajo (menor hueco libre →
  justo encima de los datos del nivel), y el anterior solo lo libera la carga
  siguiente, cuando ya ha partido el hueco (TR1: a 2150 K; TR3: a 1479 K, el
  hueco mayor baja de 6,4 a 3,4 MB). También dos bloques de 20 bytes del
  `ThreadPool_Create` (en el 0, sin daño) y, en TR3, dos bloques de ~20 K a
  4861 K sin nombre (la tabla del censo, 3072 huecos, se desbordó en la carga;
  ampliada a 6144).
- **Regla nueva** (`M_LEVEL_SMALL_MAX`, 1 KiB, `dc_level_heap.c`): los bloques
  pequeños pedidos durante la carga van a la cabeza con los de larga vida, no al
  pie; los pocos que sobreviven al nivel rellenan los huecos de los pequeños del
  nivel anterior y el tramo del nivel queda entero.
- **Composición de un nivel cargado** (lo que queda en la región): TR1 level3a
  ~2,5 MB (trozos de `GameBuf` 1,0 MB, fotogramas 424 K, mallas de objeto 222 K,
  secciones crudas 214 K, mallas de sala 189 K, tabla de ítems 184 K); TR3
  Jungle ~3,6 MB (`GameBuf` 985 K, secciones crudas 753 K, mallas de sala 663 K,
  fotogramas 406 K, mallas de objeto 236 K, ítems 184 K). El **pico** de la
  carga es mucho mayor (TR1 5398 K; TR3 6038 K): las inyecciones (4,3–5 MB
  comprimidas por juego, cada una inflada al leerla) y la conversión de
  texturas conviven con el nivel mientras carga. Se añadió la línea
  `game heap after the injections` para medirlo.
- Sin guardados (censo de TR3, build 315) Jungle, Temple y Quad Chase cargan
  sin un solo desvío (pico 6038 K de 6399 K); en la pasada con `TRX_DC_SAVE`
  (build 313) la región se llenó y hubo 47 000–150 000 desvíos: son los ~400 K
  del guardado sobre una región casi llena. Guardar en Temple Ruins en una
  consola real deja el montón seco (de ahí la copia a la VMU fallida).
- `run_flycast.ps1`: la consola de Flycast tiene 120 columnas y partía las
  líneas largas (los números de `game heap` caían a la fila siguiente); el
  lector la ensancha a 400 y limpia los sustitutos sueltos que hacían fallar
  la escritura del archivo (el log se quedaba a medias).

### Pasada final de TR1 (DC build 320, piscina de 256 K, matrices del batcher en el montón)

`TRX_DC_LEVELS='15 300' TRX_DC_SAVE=90` desde el gimnasio: **los 16 niveles y
las 9 cargas de partida** (ranura 1: recarga el gimnasio o el nivel 2, hasta
el 8B) sin caída ni desvío; 46 guardados copiados a la VMU (los demás "could
not": tarjeta llena, 16 bloques libres). La región retiene 3 bloques (0 K).
El hueco mayor baja ~90 K por **cada carga** (6271 → 5448 K en 13 cargas) y se
recupera en los niveles pequeños: la piscina de 256 K se llena durante la
carga (los pequeños transitorios doblan los ~100 K que quedan al terminar), lo
que sobra se apila bajo el muro y el mapa de solapes de salas cae debajo de lo
anterior cada vez. Piscina a **384 K** (`M_POOL_SIZE`), compilada (DC builds
323–325, discos `TRX-tr1-run2.cdi`, `TRX-tr2-run2.cdi`, `TRX-tr3-run2.cdi` en
`build/dc-minsize`) y **sin probar**: es lo primero que hay que correr.

### Para continuar (otra sesión)

1. Correr los tres discos `*-run2.cdi` (builds 323–325) con
   `run_flycast.ps1 -Hidden -Out build/dc-run-trN.txt -Seconds 900 -StopOn
   'exception [0-9a-f]{4}|Assertion|aborting|starting the disc over|\[levels\] done'`
   y resumirlos con un lector de `game heap:` por nivel (los logs de las
   pasadas anteriores están en `build/dc-run-tr*-b31*.txt` y
   `build/dc-census-tr*-b31*.txt`). Comprobar que el hueco mayor ya no baja
   por carga, que TR2 pasa Barkhang (12) y que TR3 llega al 20.
2. Si TR2 sigue cayendo en Barkhang: TAREAS 64 (pico de la carga: inyecciones
   y conversión de texturas; bloques pequeños de newlib).
3. La VMU de Flycast (`D:\Flycast\data\T-TRX00_vmu_save_A1.bin`) tiene las
   partidas de prueba (ranuras 1–5 de los tres juegos); la copia previa es
   `T-TRX00_vmu_save_A1.bin.user-backup-20261024`.
4. Los discos `*-run2.cdi` son de antes de `e139434fb` y `f3b6f427d` (cambio
   de juego en el mismo proceso, relieve nuevo): compilar y hacerlos de nuevo.
   (La fila T63 de la otra sesión pasó a T71 en PRUEBAS_PENDIENTES.md: chocaba
   con la de este bloque.)

## 24/10/2026 (tarde): avisos de juegos, expansión en el selector, resultado del guardado

- Cambiar de juego ya no reinicia la consola (ver T70): el motor acaba su hilo en vez de
  llamar a `exit()` (que caía en TR2) y `__wrap_main` lanza el juego elegido en el mismo
  proceso. Se quitó la nota que se dejaba en la tarjeta (`DCNEXT`): no hace falta; la nota
  de RAM queda solo para la recuperación tras una caída. Una ejecución dejaba puestas cosas
  que la siguiente heredaba (`m_IsExiting`, el ejecutor de fases, los interruptores de las
  pruebas, `TRX_DIR`), y eso es lo que hubo que limpiar.
- El selector previo al motor ofrece los niveles de la expansión (página 4), con el
  nivel pasado por la nota (`DC_Games_TakeStartLevel`), y L/R cambian de juego.
- El pasaporte dice cómo fue el guardado (`trx.game.dc_vmu_result()`), sin cerrar
  hasta un segundo después.

## 25/10/2026: relieve (bump) con brillo y luces de paso

- El relieve usa la dirección intermedia entre la luz y la cámara (Blinn), así que se
  mueve al mover la cámara. Una segunda pasada (`DC_BLEND_BUMP_GLOW`, suma luz blanca con
  la alfa del relieve) da el brillo; sigue la luz de la cara (cuadrado) para no verse en
  zonas oscuras. Lo plano ya no se oscurece (`M_BUMP_DIM` 0).
- Cada cara mezcla las luces que la alcanzan, por cercanía a su alcance y fuerza; las de
  paso (fogonazos, bengalas) pesan 8 veces más y llevan el relieve hasta 3 veces más
  lejos de la cámara. Alcance 5 sectores, entrada suave sobre 3.
- El relieve solo se apaga por debajo de 20 fps (`M_BUMP_MIN_FPS`).
- Valores a ajustar a ojo: `M_BUMP_DEPTH` 0,9, `M_BUMP_GLOW` 0,5. Sin probar en consola real.

## 26/10/2026: relieve (bump) — versión óptima (DC build 755)

El usuario la dio por buena ("mucho mejor") tras probar la fogata de TR3. Es el punto de
partida si algo del relieve se rompe más adelante (etiqueta `bump-optimo-b755`).

- Dirección y fuerza **por vértice** (`M_BumpAt`, `M_BumpVertex`): la PVR interpola el
  relieve por la cara, así que un lado de un suelo se ilumina distinto del otro.
- **Rejilla 4×4** (`M_EmitBumpGrid`) en los quads que alcanza una luz de paso (fogata,
  bengala, fogonazo): da el degradado radial. Dos fallos que costaron tiempo: las UV de
  los vértices nuevos sin inicializar (ruido granulado en todas las paredes) y la
  profundidad de los trozos igual a la de la cara de abajo (parpadeo píxel a píxel al
  mover la cámara; se arregla con `M_BUMP_GRID_NEAR` 1,002).
- Q (la dirección) es un byte que da la vuelta: un trozo cuyas esquinas quedan muy
  separadas toma la de su esquina más fuerte (`M_BUMP_SPLIT_Q` 96).
- El relieve se apaga con suavidad cuando la dirección se acerca a la normal de la cara
  (luz casi encima): ahí Q giraba a cada parpadeo de la llama y vibraba (`steady`).
- Luces: mezcla ponderada por alcance y fuerza; las de paso pesan 8 veces más y alcanzan
  1,5 veces más lejos; una cara cerca de una luz de paso recibe relieve aunque esté lejos
  de la cámara. Soles de sala de TR3 incluidos (el sentido de la dirección no está
  comprobado).
- Brillo (`DC_BLEND_BUMP_GLOW`): suma luz blanca por la alfa del relieve; sigue la luz de
  la cara al cuadrado para no verse en zonas oscuras.
- Se apaga solo por debajo de 20 fps; un parón (<6 fps, una carga) no cuenta, y vuelve a
  probar a los 5 s.
- Valores: `M_BUMP_DEPTH` 0,9, `M_BUMP_GLOW` 0,5, alcance 5 sectores (x3 con luz de paso).
- Pendiente: la zona del suelo donde la luz mira a la cámara sigue vibrando (el brillo
  cuando la dirección intermedia coincide con la normal).

Ajustes posteriores a la versión óptima (DC build 755): el brillo se apaga antes que el
sombreado cerca de la normal (`shine_smooth`, quita la vibración del suelo donde la luz
mira a la cámara); el relieve solo se apaga por debajo de 15 fps (`M_BUMP_MIN_FPS`, antes
20); intensidad general un 15 % menor (`M_BUMP_DEPTH` 0,765 y `M_BUMP_GLOW` 0,425).

## 26/10/2026: build y desarrollo estrictos (para trabajar con la consola real)

Flycast no emula varias cosas que la consola sí (RAM con basura, listas de la PVR con
límites, el caché del SH-4) y no hay cable serie. Lo que se añadió:

1. **Memoria con basura** (`DC_STRICT=1 bash tools/dreamcast/build.sh minsize`, en
   `build/dc-minsize-strict`): `-ftrivial-auto-var-init=pattern` para los locales, el montón
   libre relleno con 0xCD al arrancar, cada bloque de `malloc` relleno con 0xCD y cada
   `free` con 0xDD (`dc_strict.c`, ganchos en `dc_alloc_trace.c`). Lo que lee memoria sin
   escribirla falla igual en Flycast. En TR2 nivel 1 arranca y juega bien.
2. **Migas de pan y cuelgues** (`DC_Crumb` en `dc_strict.c`, siempre activo): el color del
   borde de la pantalla marca la etapa (rojo arranque, naranja juegos, amarillo lista de
   niveles, verde motor, cian cargando, azul cargado, magenta primer fotograma, negro
   jugando). Un hilo vigila los fotogramas; con 20 s sin ninguno escribe el informe en
   serie, lo dibuja en pantalla (sin comprobar: Flycast no muestra escrituras directas a
   la VRAM) y, a los 40 s, lo guarda en la VMU como `TRXLOG` (comprobado). Para probarlo:
   `DC_STRICT=1 DC_TEST_HANG=300 ...` cuelga el juego en el fotograma 300.
3. **Auditorías de límites de la PVR** (`M_AuditBins` en `pvr.c`, cada 60 fotogramas; cada
   10 en el build estricto): punteros por tesela y lista (`[bins]`), vértices no finitos,
   con profundidad no positiva o fuera de la pantalla (`[audit]` en aviso), memoria de vídeo
   libre y bytes de cada lista. La marca de uso de la pila ya estaba en `dc_watchdog.c`.
   **Hallazgo**: en el primer fotograma de TR2 nivel 1 hay hasta 76 punteros por tesela en la
   lista opaca (9 teselas sobre 16) y 109 en la de punch-through (10 sobre 16), con bloques
   de 16 palabras. Si la consola recorta por ahí, Lara (la última en dibujarse) sería lo
   primero en perderse.
4. **Avisos del compilador** (`dc_strict_warnings` en `platform/dreamcast/meson.build`):
   `dc_strict.c` compila con `-Wextra -Wshadow -Wuninitialized -Wmaybe-uninitialized
   -Wconversion -Werror`. El resto de la capa se mira a petición con
   `bash tools/dreamcast/build.sh minsize ../deps libdc_lint.a`: hoy 233 avisos (222 de
   conversiones; los demás: `strdup`, `strcasecmp` y `sbrk` sin declarar, tres formatos
   `%d` con `int32_t`, una comparación con signo).

Bloques de la PVR (DC build 765): `opb_sizes` sube a 32 palabras en la lista opaca y en la de
punch-through (translúcida sigue en 16), por los 76 y 109 punteros por tesela que dio la
auditoría. Cuesta unos 40 KB de memoria de vídeo. Sin comprobar en la consola: si Lara
vuelve a salir, era el desbordamiento; si no, mirar el color del borde y `TRXLOG` en la VMU.
