# Interstate '76 - Open Source Rewrite - Codename "Vigalante '76"

A from-scratch reimplementation of Interstate '76 Nitro Pack (Activision, 1997)
for Linux and modern Windows, Vulkan based and (hopefully) upscaled models and textures. "True" to the original binaries. Requires original game assets, I'm using the GOG nitro version.

## Approach

- **Faithful first.** Mechanics are transcribed from the original executable: the vehicle
  simulation, input bindings, cameras, lighting and asset formats follow the original's code,
  constants included. Where we use a modern method instead, it is listed in
  [`docs/DEVIATIONS.md`](docs/DEVIATIONS.md), and the faithful behaviour stays available behind
  `--legacy`.
- **Modern presentation.** Rendering may be modernised (Vulkan, higher resolutions, per-pixel
  lighting, upscaled assets). Physics and gameplay are not moddable.
- **Frame rate.** The modern mode is meant to run at any frame rate; `--legacy` keeps the
  original's frame-rate behaviour.

## Current Status — as of 2026-11-22

The live state of each system. **Updated** is when that row last changed.

| System | State | Updated |
|--------|-------|---------|
| Window + Vulkan renderer | Working — SDL2 window, Vulkan 3D pass, GPU palette lookup, depth-biased road decals | 2026-11-22 |
| Software renderer | Working — CPU rasteriser, the development fallback and A/B reference | 2026-06-21 |
| ZFS archive reader | Working — nitro.zfs, LZO1X/LZO1Y decompression, XOR decrypt | 2026-06-14 |
| VFS + PIX/PAK index | Working — nitro.zix routing, global record→pak index | 2026-07-01 |
| PCX, fonts, Smacker video | Working — loading screens, `.fnt` text, intro/credits with audio | 2026-11-22 |
| String table | Stub — the GOG release has no lang.txt | 2026-06-05 |
| Mesh cache + OEG meshes | Working — faithful GEO mesh cache (dual index, refcount, LRU); textured meshes | 2026-06-21 |
| Missions | Working — object placement, `.sdf` structures, terrain zones, roads, surface types | 2026-11-22 |
| Terrain | Working — exact height scale (12-bit × 0.1, bilinear), per-mission textures | 2026-07-01 |
| Roads | Working — the original's blocks, textures and UVs, clipped to the terrain | 2026-11-22 |
| Lighting | Working — the original's time-of-day model and `.lum` brightness response, per pixel | 2026-11-22 |
| Vehicles | Working — `.vcf`/`.vdf` chassis, `.vtf` skins, `.wdf` wheels | 2026-11-22 |
| Vehicle physics | Base model transcribed — ground handling, engine + 3-speed automatic, steering, grip per surface, suspension | 2026-11-22 |
| Input | Working — the original `input.map` / `gamekey.map` bindings and analog key stepping | 2026-11-22 |
| Cameras | External track camera transcribed; other views not yet | 2026-11-22 |
| Collisions | Not started (props, vehicles, drivable objects) | — |
| Damage, weapons, AI | Not started | — |
| Mission scripting | Not started | — |
| Frontend / shell | Not started — startup goes straight to gameplay | — |
| Audio | Not started (videos only) | — |
| Multiplayer | Non-goal for now | — |

## Next — as of 2026-11-22

1. Remaining vehicle physics: landing response, bump jolt, damage and flat tyres, the AI
   branch, the distant (off-screen) simulation.
2. Frame-rate independence for the modern mode (key ramps, suspension filter, camera).
3. Collisions with props and other vehicles, transcribed from the original.
4. Stage a real mission: the player's vehicle and spawn from the mission script.

## Progress Snapshots

One line per milestone, newest first. The full detail is in the git history and `docs/`.

- **2026-11-22** — Drivable: the vehicle simulation transcribed from the original, with the
  original key bindings, track camera, time-of-day lighting and road drawing. World
  handedness corrected. `--legacy` switch and the deviation register added. Mod layer added.
- **2026-07-01** — All mesh textures resolve, including `.vqm` pak records and `.m16`.
- **2026-06-30** — Terrain height and object placement solved; missions render correctly.
- **2026-06-21** — 3D at last: software and Vulkan backends, textured meshes, world viewer,
  first roads.
- **2026-06-17** — PIX/PAK index and the scene viewer; multi-part structures placed.
- **2026-06-14** — Mission placement chain reversed; OEG mesh decode and mesh viewer.
- **2026-06-05** — Project start: ZFS/VFS, PCX, fonts, Smacker video, Vulkan palette blit.

## Goals

- Native Linux and modern Windows (no compatibility layers)
- Faithful to the original — not a reimagining
- Clean, readable C; what we learned from reversing lives in `docs/`
- Original assets required; open-source asset repo support planned

## Non-Goals (for now)

- Multiplayer
- Full editor / asset pipeline tooling

## Game Files

No game assets are included; they are copyrighted. You need your own install of Interstate '76
Nitro Pack (developed against the GOG release). Point `--assets` at the install folder; nothing
needs copying or extracting.

From the install, the engine reads exactly these 10 files. Everything else it uses (vehicles,
meshes, textures, terrain textures, physics and engine data) comes from inside `nitro.zfs`.

| File in the install | What it is | |
|---------------------|------------|--|
| `nitro.zix` | Archive index | required |
| `nitro.zfs` | The main archive | required |
| `miss8/N01.CBT` | The default mission | required |
| `miss8/N01.TER` | Its terrain | required |
| `input.map` | Original key bindings (held actions) | required |
| `gamekey.map` | Original key bindings (commands) | required |
| `base6x7.fnt` | HUD font | required |
| `smk/INTROF01.SMK` | Intro video | optional, skipped if missing |
| `smk/CREDF01.SMK` | Credits video | optional, skipped if missing |
| `addon/LOADGAME.PCX` | Loading screen | optional, skipped if missing |

This list comes from logging every file opened during a full run against the GOG install
(2026-11-22). It grows as more of the game is implemented.

## Building

```bash
# Dependencies (Arch Linux)
sudo pacman -S sdl2 sdl2_ttf vulkan-icd-loader vulkan-headers glslang cmake lzo

# Dependencies (Ubuntu/Debian)
sudo apt install libsdl2-dev libsdl2-ttf-dev libvulkan-dev glslc cmake build-essential liblzo2-dev

# Build (the i76 binary is written to the repository root)
mkdir build && cd build
cmake ..
make
cd ..

# Run
./i76 --assets "/path/to/Interstate 76 Nitro Pack"
```
Without `--assets`, assets are read from the current directory.
i76 defaults to the overhauled hardware (Vulkan) renderer. For the software renderer, use `--render sw`.

There is no complete game yet. A plain run plays the intro videos and loading screen(For your nostalgia), then
opens the world viewer on `miss8/N01.CBT`: an overview of the level terrain, then after a few
seconds will drop you into the driving mode (F toggles free-fly / drive). 
This "worldview" is my test bed. Everything here is real engine, as built at the time of release.


Assets are your own i76 packages or install. I will not provide original assets here. See [Game Files](#game-files)
for exactly which files are needed.

## Running

| Option | Effect |
|--------|--------|
| (none) | Intro videos, loading screen, then the world viewer |
| `--assets <path>` | Game asset directory (default: current directory) |
| `--legacy` | Faithful behaviour for every listed deviation ([`docs/DEVIATIONS.md`](docs/DEVIATIONS.md)) |
| `--mods <path>` | Mod folder (default `<assets>/mods`), see [`docs/MODDING.md`](docs/MODDING.md) |
| `--no-mods` | Original assets only |
| `--worldview [mission]` | Straight to the world viewer, no intros (default `miss8/N01.CBT`) |
| `--render sw` | World viewer on the software rasteriser (default: Vulkan, `hw`) |
| `--sceneview [mission]` | Wireframe object + terrain viewer |
| `--meshview` | Browse decoded meshes |
| `--gameloop` | The game state-machine scaffold (placeholder loop) |

### World viewer keys

The viewer opens on an overview of the mission and hands over to driving after a few
seconds. The keys for the current mode are listed on screen, top left. Driving uses the
original game's bindings only; the viewer's own keys (marked *viewer*, orange on screen) sit
on keys the original leaves free.

The original bindings are read from `input.map` and `gamekey.map` in the game folder. They are
plain text, so a keyboard without a numpad can be rebound there.

**Drive**

| Key | Action |
|-----|--------|
| Numpad arrows | Throttle, brake, steer |
| Arrow keys | Look around |
| S | Engine on / off |
| Z | Handbrake |
| `.` / `,` | Shift up / down |
| TAB | Reverse direction |
| PgUp / PgDn | Camera distance |
| F1 – F11 | Change view (only the chase cam exists so far) |
| F | Fly (*viewer*) |

**Fly** (all *viewer* keys)

| Key | Action |
|-----|--------|
| F | Drive the car |
| W A S D | Move |
| Q / E | Down / up |
| Arrows | Look |
| Shift | Faster |
| TAB | Software / Vulkan renderer |
| P | Terrain height multiplier (debug) |

**Orbit**, to inspect the car (all *viewer* keys)

| Key | Action |
|-----|--------|
| Arrows / W A S D | Rotate |
| Q / E | Zoom |
| Shift | Faster |
| O | Back |

**Any mode** (*viewer* keys)

| Key | Action |
|-----|--------|
| O | Orbit the car |
| Shift+L | Time of day +3 hours |
| ESC | Quit |

## Project Structure

```
i76/
├── src/
│   ├── main.c                  # Entry point and outer game loop
│   ├── platform/               # Window, events, timing, memory (SDL2)
│   ├── engine/
│   │   ├── cmdline.c/h         # Command-line parser
│   │   ├── fidelity.c/h        # --legacy switch
│   │   ├── gamestate.c/h       # Game state machine
│   │   ├── loop.c/h            # Gameplay frame loop (placeholder)
│   │   ├── input.c/h           # Per-frame input snapshot
│   │   ├── keybind.c/h         # input.map / gamekey.map bindings (+ keybind_tables.c)
│   │   ├── world.c/h           # Simulated world seam (entity store goes here)
│   │   ├── zfs.c/h, vfs.c/h    # Archives and the virtual filesystem
│   │   ├── pix.c/h             # PIX/PAK record index
│   │   ├── fs.c/h              # Asset root + case-insensitive file open
│   │   ├── mods.c/h            # Presentation-only mod layer
│   │   ├── meshcache.c/h       # GEO mesh cache
│   │   ├── geomesh.c/h         # OEG mesh decode
│   │   ├── texture.c/h         # .map / .tmt textures
│   │   ├── scene.c/h           # Mission loader: objects, terrain, roads, surfaces
│   │   ├── lighting.c/h        # Time-of-day global lighting
│   │   ├── vehicle.c/h         # Vehicle models, skins and wheels
│   │   ├── vehsim.c/h          # Vehicle simulation
│   │   ├── trackcam.c/h        # External track camera
│   │   ├── worldview.c/h       # 3D world viewer and driving test
│   │   ├── sceneview.c/h       # Wireframe scene viewer
│   │   ├── meshview.c/h        # Mesh viewer
│   │   ├── font.c/h, pcx.c/h   # Fonts and PCX images
│   │   ├── video.c/h           # Smacker video playback
│   │   └── strlookup.c/h       # String table (stub)
│   └── render/
│       ├── render.h            # Renderer interface
│       ├── render_vk.c         # Vulkan backend
│       ├── softraster.c/h      # Software rasteriser
│       └── shaders/            # blit + mesh shaders
├── tools/
│   ├── asset_browser.c         # GUI asset browser — file list + PCX/WAV preview (SDL2_ttf)
│   ├── zfs_dump.c              # Extract / list nitro.zfs
│   ├── smk_dump.c              # Inspect Smacker .smk files
│   └── ReapplyI76Renames.java  # Ghidra script: reapply our function names
├── lib/libsmacker/             # Smacker decoder (third-party)
├── docs/
│   ├── REVERSING.md            # Reversing notes: formats, functions, globals, physics
│   ├── DEVIATIONS.md           # Every departure from the original, and its --legacy status
│   └── MODDING.md              # Mod folder layout and rules
└── CMakeLists.txt
```

## Tools

```bash
# List all files in nitro.zfs (shows stored + decompressed sizes)
./tools/zfs_dump list "/path/to/nitro.zfs"

# Extract entire archive (decompresses each entry)
./tools/zfs_dump extract "/path/to/nitro.zfs" out/

# Extract one file (decompressed)
./tools/zfs_dump get "/path/to/nitro.zfs" FILENAME.EXT out.bin
```

`zfs_dump` shares the engine's ZFS reader, so `get`/`extract` write the real
**decompressed** bytes (LZO1X/LZO1Y), not the stored form.

## Credit
Reach out if you feel like you deserve some credit, this is a deep community and I don't know who is behind many of the resources.

"That Tony" did some excellent work deserialising the game binaries, ZFS and VFS systems, much inspiration came from his work. (2007??)

David Hopkinson  aka "Hopper" - provided a great insight with his "Hopper's Guide" which delves deep into the model and texture mapping side of things. (1999??)

A big nod to therealjkvalentine and his [i76-everywhere](https://github.com/therealjkvalentine/i76-everywhere) project, which upgrades the original binaries. We considered that route ourselves before going with a full rebuild, and it's great to see it done so well. It's a really comprehensive project with plenty of great ideas and an ambitious engine roadmap, and it has come together impressively fast. Well worth checking out.

## Reversing Notes

See [`docs/REVERSING.md`](docs/REVERSING.md) for everything deduced from the original
binary: entry points, game state machine, ZFS archive format, VFS layer and asset load
chain, the GEO mesh cache and OEG mesh format, mission files and object placement, terrain,
roads, lighting, vehicles, input bindings, cameras, the vehicle physics, and all known
globals and function addresses. Claims not yet confirmed are marked `???`.

## Original Binary

- **Game**: Interstate '76 Nitro Pack (Activision, 1997)
- **Platform**: Win32 x86, MSVC-compiled
- **Reversing tool**: Ghidra
