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
