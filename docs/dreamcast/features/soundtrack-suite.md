# Feature aparcada: selector de banda sonora (Original / Tomb Raider Suite)

Reactivada el 10/11/2026 (rama `full60fps`), sin probar aún. Cancelada el 09/11/2026 por petición del usuario; el audio quedó **exactamente como antes**.
El trabajo está guardado en [soundtrack-suite.patch](soundtrack-suite.patch)
(`patch -p1 --ignore-whitespace < docs/dreamcast/features/soundtrack-suite.patch` desde la raíz).

Qué hacía:
- Objeto «Sound» en el anillo de opciones (`nolua_ring.c`) con una página de una fila,
  «Soundtrack» (Original / TR Suite), en `nolua_settings.c`.
- `audio.dc_suite_music` en `map.def`/`types.h`; formato 12 de `trdctriopt` para guardarlo.
- `music/common.c`: elige `music_suite` junto a `music` y `Music_ReloadSoundtrack()` cambia la
  música que suena al momento (cierra los streams, reinicia el reproductor y reanuda la pista
  de ambiente y la normal). El menú de la consola no avisa de los cambios de ajuste
  (`Config_Update`), así que la fila llama a la recarga directamente.
- `make_disc.sh`: convertía las pistas numeradas de la Suite (`TRACKS/*.ogg` y `TRACKS/EN`) a
  ADPCM en `music_suite`, rellenando con las originales las que faltan; y para TR1 clásico
  añadía las pistas 2-57 del CD (`Tomb Raider (v1.6) (Track NN).bin`, PCM crudo 44,1 kHz).

Hallazgos:
- Los `.ogg` con nombre de la Suite (CANYON, LIFT, MANSION, PRISON, SNOW, VISION, END) son el
  audio de los vídeos, no las ambientales.
- El nivel 1 de TR1 pide la pista 57 (ambiental); los mp3 de Steam solo traen 2-10.
- Quedó sin resolver: con el disco de TR1 de esa prueba no se oían las ambientales (ni en
  Original ni en Suite) ni las pisadas de Lara; el registro no mostraba errores de sonido.
