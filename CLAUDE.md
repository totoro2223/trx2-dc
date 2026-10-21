# TRX para Dreamcast

Este repositorio es el port de TRX a Sega Dreamcast (rama `dreamcast`).
Antes de empezar, lee [docs/dreamcast/TRASPASO.md](docs/dreamcast/TRASPASO.md):
entorno, ciclo de compilación y pruebas, arquitectura, estado y pendientes.
Las tareas listas para tomar están en
[docs/dreamcast/TAREAS.md](docs/dreamcast/TAREAS.md), y lo ya hecho, en
[docs/dreamcast/REGISTRO.md](docs/dreamcast/REGISTRO.md).

Reglas básicas:

- El usuario habla en español; haz las preguntas de una en una.
- Nunca hagas commit de: CDIs ni otros productos de compilación (`build/`),
  herramientas descargables (DreamSDK/KallistiOS, Flycast, `deps/`), ni
  nada con copyright del juego (niveles `*.TR2`, `MAIN.SFX`, texturas,
  sonidos, FMV, `textures.vq`, capturas).
- Compila tras cada cambio
  (`.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh minsize"`).
  Genera un CDI solo si el usuario lo pide o si necesitas probar en Flycast,
  y di siempre qué "DC build N" es.
- No toques un Flycast que el usuario tenga abierto.
- El código de Dreamcast va en `src/platform/dreamcast/` y
  `tools/dreamcast/`; en código compartido, protege los cambios con
  `#if __DREAMCAST__`.
