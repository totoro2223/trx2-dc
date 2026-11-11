<div align="center">
<h1>trdreamcasttrilogy – Tomb Raider I-III for Sega Dreamcast</h1>

</div>

<hr/>

**trdreamcasttrilogy** is a **Sega Dreamcast** port of
[TRX](https://github.com/LostArtefacts/TRX), the open-source reimplementation
of **Tomb Raider 1, 2 and 3** by LostArtefacts. It runs on
[KallistiOS](https://github.com/KallistiOS/KallistiOS) and boots from a disc
image (CDI) in **Flycast** and on real hardware with **GDEMU**.

The engine and the gameplay are TRX's. This repository adds the Dreamcast
platform layer (PowerVR video, AICA audio, controller, VMU, disc loading), a
renderer of its own, and the tools that build the discs. TRX is a modern engine
and the Dreamcast is 1998 hardware, so the port needed many changes to the
engine itself, not only a new platform layer (see
[How this port was made](#how-this-port-was-made)).

## Features

- **TR1, TR2 and TR3**, with their expansions (Unfinished Business, Golden
  Mask, Lost Artefact), each game on its own disc or all three on one disc.
- **Switching between the games from the main menu**, like the Tomb Raider
  I-III Remastered: on the three-game disc, `Up` and `Down` on the main menu
  go to the previous and the next game, and the New Game page of the passport
  offers that game's expansion.
- **Display modes**: original, **16:9 anamorphic**, and **Original 60** (the
  game is drawn at 60 frames per second, with the game logic running at 30 on
  its own thread).
- Saved games and options on the **memory card (VMU)**, with an icon and a
  notice on the card's screen.
- **Modern shadows for Lara and the enemies**, in place of the original's
  simple blob: **soft shadows** (the shape of the character, rendered to a
  texture and blurred at the edges) or **volume shadows** (real shadow volumes,
  cast by the PowerVR's modifier volumes). Chosen in the options ring; enemy
  shadows have their own option.
- **Quick save and quick load** from the controller (`L + R + Start` and
  `L + R + D-pad Down`), written to the memory card with a "SAVING... / GAME SAVED"
  notice and a beep.
- **Level selector**: a disc made without a starting level opens on a list of
  levels, to jump straight to any of them.
- **VMU screen in game** (`L + R + B`): logo, level statistics and a secret
  finder, on the memory card's own display.
- **Diagnostics**: test discs keep the engine's log on the serial port
  (Flycast saves it to a file), with per-frame timing lines and a "DC build N"
  version stamp; the strict build shows a hang report on screen with the last
  stages the game went through.
- **Soundtrack selector**: choose between the **Original** music and the
  **Tomb Raider Suite** from the options ring (Sound page, "Soundtrack" row);
  the change applies at once. The Suite tracks come from the Remastered data
  when the disc is built (tracks it lacks fall back to the originals).
- Music, sound effects and MPEG-1 movies on the disc.
- Menus of the game for the Dreamcast's own options (display mode, filters,
  shadows, bump relief, and others).

## Technical highlights

The Dreamcast has a 200 MHz SH-4, 16 MB of main RAM, 8 MB of video RAM and
2 MB of sound RAM. Much of the work in this port is about fitting the engine
in that.

### SH-4 assembly

- The per-frame hot paths are hand-written in **SH-4 assembly**, using the
  FPU's vector instructions (`ftrv`, `fipr`, `fsrra`) and `pref`: vertex
  transform, lighting and fog for rooms, objects and lit meshes; face emission
  to the PowerVR; particles, sparks, rain, snow and water; portal visibility
  windows; instance culling; shadow volumes; bump relief; and the
  item and creature movement and animation updates.
- Each routine keeps its **C version as the reference**. A verify build runs
  both and compares every result (millions of vertices and indices per test
  level), and every routine has a runtime switch on test discs, so a faulty one
  can be bisected on real hardware.

### Original 60 mode

- The game logic runs at 30 Hz on its own thread while a **render thread**
  draws every vertical blank, so a slow tick no longer holds a frame back.
  The world is staged once per tick; the frame after it is drawn blended
  halfway back to the previous tick, and the next one replays the staged tick.
- Rooms are baked into **per-chunk vertex batches in the SH-4's on-chip RAM
  (OCRAM)** and drawn without re-transforming unchanged vertices. Static
  meshes are merged into their room at load time, and object lighting is cached
  by light key.
- Fog is done with the PowerVR's **fog table**, and dynamic lights are
  filtered per chunk, so only the chunks a light reaches pay for it.

### Rendering

- A **PowerVR renderer written for this port**, in place of the engine's
  OpenGL one: opaque, punch-through, translucent and modifier-volume lists,
  geometry sent through the **store queues**, and a cache of PowerVR headers.
- **Twiddled and VQ-compressed textures** (the texture pages are compressed at
  disc-build time), mipmaps and an environment map for weapon reflections.
- Shadows two ways, for Lara and for enemies: a **soft shadow** rendered to a
  texture (one shadow map shared by Lara and up to four enemies), or **shadow
  volumes** built from the character's bounding box and drawn with the
  PowerVR's modifier volumes (the volume is built in SH-4 assembly).
- **Bump mapping** on nearby faces, **cel shading**, per-vertex coloured fog
  and a **16:9 anamorphic** mode.

### Audio

- Sound effects are re-encoded at disc-build time to the **AICA's own Yamaha
  ADPCM** (TR2's `main.sfx` goes from 7.96 MB to 1.97 MB) and copied straight to
  sound RAM without decoding, and an index lets each level read only its own
  samples.
- Music is converted to ADPCM too, and the AICA plays it on its own.

### Fast loading

- Files read together are **laid out together on the disc** (`mkisofs -sort`),
  which matters most on the GD-ROM and GDEMU.
- Samples are read by index, textures come precompressed, and the level heap
  is sized by bisection instead of failing allocations one by one.
- Disc reads go through a PIO driver; a DMA path exists as an option.

### Memory

Sixteen megabytes is the main limit, so the data is packed: 28-byte vertices
and 16-bit indices, animation frames stored as three 10-bit angles in one
word, and texture pages in VQ. The default build has **no Lua**: the HUD, the
option ring, the passport and the settings screens are written in C, which
frees memory for the level. A lock-free arena serves the Lua build.

### Tooling

- **Benchmark mode** that runs the game's own demo and saves a timing report
  per build; per-frame timing lines in the log; a sampling profiler; allocation
  tracing; and a watchdog that reports stalls.
- Debug switches that turn each optimisation off at run time, so the cost and
  the correctness of every one can be measured.

All performance figures in the documentation were measured in Flycast; real
hardware is usually slower and is measured separately.

## Controls

Layout with the stock Dreamcast controller. The engine's default controller
bindings apply; `L` and `R` are analog triggers read as buttons past half
travel. **`L` is a modifier (a shift key)**: held with another button it
gives that button's second function (`L + Y` crouch, `L + A` sprint, `L + R`
look, and the `L + R` shortcuts below).

| Button | Action |
|---|---|
| Analog stick | Move and turn |
| D-pad | No function in game |
| `A` | Action (grab, push, pick up, confirm in menus) |
| `X` | Jump |
| `B` | Roll (back in menus) |
| `Y` | Draw / holster weapons |
| `R` (held) | Walk |
| `L + R` | Look around |
| `Start` | Pause |
| `L + Y` | Crouch (TR3) |
| `L + A` | Sprint (TR3) |
| `L + R + Start` | Quick save |
| `L + R + D-pad Down` | Quick load |
| `L + R + B` | Switch the VMU screen (logo / statistics / secret finder) |
| `L + R + X + Y` | End the level (only with the `flow.cheat_keys` option on) |
| `A + B + X + Y + Start` | Back to the main menu |

Pads with extra buttons (arcade sticks, the twin-stick) use `D` for the
inventory and `Z` / `C` for the stick clicks (change target / crouch).

## What you need

**The repository contains no game data.** You need your own copy of Tomb
Raider I-III (Steam, GOG or the Remastered edition). The disc script takes the
levels, sounds, music and movies from it and puts them in the CDI; none of it
goes into git.

To build:

- [DreamSDK](https://dreamsdk.org/) (KallistiOS and the `sh-elf` toolchain)
  with the `zlib` and `sh4zam` ports installed.
- The third-party dependencies in a `deps/` folder next to the repository
  (uthash, PCRE2, Lua and libmpeg). The exact list is in
  [docs/dreamcast/TRASPASO.md](docs/dreamcast/TRASPASO.md).
- Optional: `ffmpeg`, to convert the music and the loading pictures.

## Building and making a disc

On Windows, from the repository root (PowerShell):

```powershell
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh minsize"
```

A disc of one game (`TRX_DC_GAME` picks `tr1`, `tr2` or `tr3`):

```powershell
.\tools\dreamcast\dcsh.ps1 "TRX_DC_GAME=tr2 bash tools/dreamcast/make_disc.sh build/dc-minsize '<TR2 DATA folder>' build/obj/tr2.cdi"
```

A disc with all three games:

```powershell
.\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/make_disc_all.sh build/dc-minsize build/obj/trilogy.cdi"
```

To try a disc in Flycast, a launcher runs it and keeps the serial output:

```powershell
.\tools\dreamcast\run_flycast.ps1 -Cdi build\obj\tr2.cdi -Seconds 60
```

The environment variables of `make_disc.sh` (mode, starting level, music,
movies) are documented at the top of the script itself.

## Layout

| Folder | Contents |
|---|---|
| `src/trx/` | The TRX engine (shared with the upstream project) |
| `src/platform/dreamcast/` | The Dreamcast platform: video, audio, controller, VMU, loading |
| `tools/dreamcast/` | Build, disc creation and the Flycast launcher |
| `docs/dreamcast/` | Documentation of the port |

## Documentation

The port's documentation is written in Spanish.

- [TRASPASO.md](docs/dreamcast/TRASPASO.md): environment, workflow and
  architecture. The place to start.
- [PROBADO.md](docs/dreamcast/PROBADO.md): what has been tested and the
  result.
- [TAREAS.md](docs/dreamcast/TAREAS.md): what is still pending.
- [REGISTRO.md](docs/dreamcast/REGISTRO.md): detailed history.
- [PLAN_60_FIJO.md](docs/dreamcast/PLAN_60_FIJO.md): the Original 60 mode.

## How this port was made

The games are old (1996-1998), but **TRX is a modern engine**, written for
today's PCs: an OpenGL 3 renderer, SDL2, a Lua scripting layer for the HUD,
menus and level logic, a threaded loader, 64-bit arithmetic, current C and
plenty of RAM to spare. The Dreamcast has a 200 MHz SH-4, 16 MB of RAM and a
fixed-function PowerVR, so **a great deal had to change** to fit the engine to
its architecture: the OpenGL renderer was replaced by a PowerVR one, SDL2 by a
thin compatibility layer over KallistiOS, and Lua became optional, with the
interface rewritten in C by default. Memory use, data layouts, the main loop and
the hot paths were reworked throughout (see Technical highlights above).

This port was developed with **[Claude](https://claude.com/claude-code)**
(Claude Code) as the coding agent, working with the maintainer. Two kinds of
reverse engineering fed the optimisation work:

- **REA**, a reverse-engineering toolkit the agent drives (with Ghidra), was
  used on the original PC executables of TR1, TR2 and TR3 to check how the
  original engine does things before changing them.
- The **PlayStation version** of the games was decompiled too (TR1 and TR3,
  with Ghidra). The PSX runs these games at about 30 fps on a 33 MHz CPU, so
  its methods, such as how it culls, lights and fogs, were a guide to what is
  affordable on the Dreamcast. Only addresses and figures were recorded; no
  decompiled code or game data is in this repository.

The findings are written up in `docs/dreamcast/INFORME_REA_TR123.md`,
`REA_TR1_PSX.md` and `REA_TR3_PSX.md`. The rule of thumb that came out of it:
**port the original's method, do not invent a new one.**

## Building with an AI coding agent

You can continue the port with an AI coding agent (Claude Code, OpenAI Codex,
or similar). What makes it work:

1. **Give it the context first.** [CLAUDE.md](CLAUDE.md) holds the project's
   rules and points to [TRASPASO.md](docs/dreamcast/TRASPASO.md) (environment,
   workflow, architecture). Agents that read a different file, such as
   `AGENTS.md`, can be told to read `CLAUDE.md` and `TRASPASO.md` first.
2. **Make it check what was already tried.** [PROBADO.md](docs/dreamcast/PROBADO.md)
   lists what worked, what failed and what is unseen, so the agent does not
   repeat a dead end. [TAREAS.md](docs/dreamcast/TAREAS.md) lists what is left.
3. **Keep the loop short.** Build after every change
   (`build.sh minsize`); make a disc only when you need to run it. Run tests in
   Flycast on a hidden desktop with the sound muted, and read the serial log
   the launcher saves.
4. **Measure, do not guess.** The benchmark mode, the per-frame log lines and
   the runtime switches give the agent numbers to compare. Ask it to verify an
   optimisation against the C reference before keeping it.
5. **Keep the repository clean.** Never commit game data, discs or anything
   under `build/`. The agent should record results in `PROBADO.md` and clear
   the finished task from `TAREAS.md`.
6. **Test on hardware.** The agent can only see Flycast. Real-console results
   are usually worse and are the ground truth; feed them back to it.

## Status

Work in progress. Development and day-to-day testing are done in Flycast, but
the **final tests were done on a real Dreamcast** (with GDEMU). Performance on
real hardware is usually worse than in the emulator, so the console results are
the ones that count. What is known and what is pending are in `PROBADO.md` and
`TAREAS.md`.

### Not implemented, or left off, because they failed on real hardware

Some ideas worked in the emulator but not on the console. They are not part of
the default build:

- **Animated objects drawn as one rig** (one mesh per object with a matrix per
  bone): the console reset in TR1 level 1 with it on. It is off by default.
- **Three SH-4 assembly routines** (vector turning, sphere-pair tests and
  collision boxes): disabled after they caused problems on the console. The C
  versions run instead.
- **Vertices sent through the store queues, and `pref` in the transform**: no
  gain on the console (Jungle stayed at 17-19 fps), so they stay off.
- **The PowerVR punch-through list for keyed texels**: the keyed texels came
  out black on the console in every variant, so they are blended through the
  translucent list instead.
- **GD-ROM reads by DMA** exist as an option, but are off by default; reads go
  through the slower PIO path that works everywhere.

Several of the tuning ideas that gave nothing measurable are listed, with their
numbers, in `PROBADO.md`.

## Reporting bugs

See [BUG_REPORTING.md](BUG_REPORTING.md) and the
[code of conduct](CODE_OF_CONDUCT.md). Always say whether the problem is in
Flycast or on real hardware, and the version ("DC build N") shown in the log.

## License and credits

TRX is under the **GNU GPL v3** (see [COPYING.md](COPYING.md)), and this port
keeps it.

- [TRX](https://github.com/LostArtefacts/TRX) by LostArtefacts and its
  community: the engine and the gameplay.
- [KallistiOS](https://github.com/KallistiOS/KallistiOS) and DreamSDK: the
  system and the tools for the Dreamcast.
- Tomb Raider is a trademark and work of Square Enix / Crystal Dynamics / Core
  Design. This project is not affiliated with them and includes none of their
  data.
