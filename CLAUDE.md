# TRX para Dreamcast

Este repositorio es el port de TRX a Sega Dreamcast (rama `dreamcast`).
Antes de empezar, lee [docs/dreamcast/TRASPASO.md](docs/dreamcast/TRASPASO.md):
entorno, ciclo de compilación y pruebas, arquitectura, estado y pendientes.

Reglas básicas:

- El usuario habla en español; haz las preguntas de una en una.
- Los datos del juego (`*.TR2`, `MAIN.SFX`, etc.) nunca entran en el repo.
- Compila tras cada cambio
  (`.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh minsize"`).
  Genera un CDI solo si el usuario lo pide o si necesitas probar en Flycast,
  y di siempre qué "DC build N" es.
- No toques un Flycast que el usuario tenga abierto.
- El código de Dreamcast va en `src/platform/dreamcast/` y
  `tools/dreamcast/`; en código compartido, protege los cambios con
  `#if __DREAMCAST__`.
