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

## Current Status

| System | State |
|--------|-------|
| Window + Vulkan renderer | Working — SDL2 window, Vulkan 3D pass, GPU palette lookup, depth-biased road decals |
| Software renderer | Working — CPU rasteriser, the development fallback and A/B reference |
| ZFS archive reader | Working — nitro.zfs, LZO1X/LZO1Y decompression, XOR decrypt |
| VFS + PIX/PAK index | Working — nitro.zix routing, global record→pak index |
| PCX, fonts, Smacker video | Working — loading screens, `.fnt` text, intro/credits with audio |
| String table | Stub — the GOG release has no lang.txt |
| Mesh cache + OEG meshes | Working — faithful GEO mesh cache (dual index, refcount, LRU); textured meshes |
| Missions | Working — object placement, `.sdf` structures, terrain zones, roads, surface types |
| Terrain | Working — exact height scale (12-bit × 0.1, bilinear), per-mission textures |
| Roads | Working — the original's blocks, textures and UVs, clipped to the terrain |
| Lighting | Working — the original's time-of-day model and `.lum` brightness response, per pixel |
| Vehicles | Working — `.vcf`/`.vdf` chassis, `.vtf` skins, `.wdf` wheels |
| Vehicle physics | Base model transcribed — ground handling, engine + 3-speed automatic, steering, grip per surface, suspension |
| Input | Working — the original `input.map` / `gamekey.map` bindings and analog key stepping |
| Cameras | External track camera transcribed; other views not yet |
| Collisions | Not started (props, vehicles, drivable objects) |
| Damage, weapons, AI | Not started |
| Mission scripting | Not started |
| Frontend / shell | Not started — startup goes straight to gameplay |
| Audio | Not started (videos only) |
| Multiplayer | Non-goal for now |

## Next

1. Remaining vehicle physics: landing response, bump jolt, damage and flat tyres, the AI
   branch, the distant (off-screen) simulation.
2. Frame-rate independence for the modern mode (key ramps, suspension filter, camera).
3. Collisions with props and other vehicles, transcribed from the original.
4. Stage a real mission: the player's vehicle and spawn from the mission script.

## Goals

- Native Linux and modern Windows (no compatibility layers)
- Faithful to the original — not a reimagining
- Clean, readable C; what we learned from reversing lives in `docs/`
- Original assets required; open-source asset repo support planned

## Non-Goals (for now)

- Multiplayer
- Full editor / asset pipeline tooling

## Building

```bash
# Dependencies (Arch Linux)
sudo pacman -S sdl2 sdl2_ttf vulkan-icd-loader vulkan-headers glslang cmake lzo

# Dependencies (Ubuntu/Debian)
sudo apt install libsdl2-dev libsdl2-ttf-dev libvulkan-dev glslc cmake build-essential liblzo2-dev

# Build
mkdir build && cd build
cmake ..
make

# Run from the game folder (assets alongside the binary)
./i76

# Or point at your asset directory
./i76 --assets "/path/to/Interstate 76 Nitro Pack"
```

## Running

| Option | Effect |
|--------|--------|
| `--assets <path>` | Game asset directory (default: current directory) |
| `--legacy` | Faithful behaviour for every listed deviation ([`docs/DEVIATIONS.md`](docs/DEVIATIONS.md)) |
| `--mods <path>` | Mod folder (default `<assets>/mods`), see [`docs/MODDING.md`](docs/MODDING.md) |
| `--no-mods` | Original assets only |
| `--worldview [mission]` | 3D world viewer and driving test (default `miss8/N01.CBT`) |
| `--render hw` | World viewer on the Vulkan renderer (default: software) |
| `--sceneview [mission]` | Wireframe object + terrain viewer |
| `--meshview` | Browse decoded meshes |

World viewer keys:

| Key | Action |
|-----|--------|
| SPACE | Toggle free-fly / drive |
| Free-fly: W A S D, Q E, arrows, Shift | Move, down/up, look, faster |
| Drive | The original `input.map` bindings, plus W A S D |
| O | Orbit camera around the car (W A S D / arrows orbit, Q E zoom) |
| TAB | Software / Vulkan renderer (outside drive mode; in drive mode TAB is reverse) |
| L | Step the time of day by 3 hours |
| P | Cycle a debug terrain height multiplier |

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
