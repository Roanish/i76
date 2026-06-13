# Interstate '76 - Open Source Rewrite - Codename "Vigalante '76"

A from-scratch reimplementation of Interstate '76 Nitro Pack (Activision, 1997)
for Linux and modern Windows, Vulkan based and (hopefully) upscaled models and textures. "True" to the original binaries. Requires original game assets, I'm using the GOG nitro version.

## Current Status

| System | State |
|--------|-------|
| Window + Vulkan renderer | Working — SDL2 window, full Vulkan pipeline, GPU palette lookup |
| PCX loader | Working — 8-bit palettised, reads via VFS |
| Smacker video | Working — intro/credits play with audio (libsmacker + SDL2 audio queue) |
| ZFS archive reader | Working — reads nitro.zfs, LZO1X/LZO1Y decompression, XOR decrypt |
| VFS layer | Working — parses nitro.zix, routes reads to ZFS or loose files |
| Font system | Working — loads `.fnt` via VFS, draws text into 8-bit indexed buffer |
| String table | Stub — `StrLookup` returns NULL; GOG release has no lang.txt |
| Game state machine | Working — each state owns its own inner loop; dispatcher in `main.c` |
| Gameplay frame loop | Working — fixed 60 Hz timestep sim + interpolated render, input snapshot |
| Mesh cache | Working — faithful GEO mesh cache (dual index, refcount, LRU); self-test passes |
| GEO/OEG mesh decode | Working — decodes OEG meshes (verts + faces) from decompressed assets (`geomesh.c`) |
| Mesh viewer | Working — `./i76 --meshview` browses decoded meshes as a rotating software wireframe |
| Game world / objects | Reversed (not yet built) — mission→ODEF→OBJ placement chain + entity creator located; `world.c` seam still a debug marker |
| 3D renderer | Not started — Vulkan backend only does a fullscreen 8-bit blit (viewer rasterizes wireframe on the CPU) |
| Audio | Not started |
| Terrain / world | Not started |
| Vehicles / physics | Not started |
| AI / combat | Not started |
| Multiplayer | Not started |

## Immediate TODO

The mission→object placement chain is now reversed (see `docs/REVERSING.md`
"Mission file & object placement"); next steps build on it:

1. **Object placement format** — dump the `OBJ` sub-chunk table (`DAT_00504958`, 6 entries)
   to pin the exact transform/field layout, so object positions can be parsed straight
   out of a mission file (and the viewer extended into a *scene* viewer).
2. **Entity store** — build out `world.c` against the reversed creator (`FUN_00453d50`,
   "cannot create entity") + object allocator (`FUN_004b3e20`): the live object struct
   and the list `world_tick` should iterate.
3. **Minimal 3D renderer** — the Vulkan backend only does a fullscreen blit; a real 3D
   pass replaces the CPU wireframe in the mesh viewer and is the path to drawing scenes.

Also outstanding:

4. Reverse NITSHELL.DLL callback table (32 entries at `&local_80` in `game_session_run`) — needed to write our own ShellMain replacement
5. Stub our own ShellMain that returns `shell_result=2` with a hardcoded scenario name, so we can reach the gameplay state
6. Wire font selection by video mode (`DAT_005fb0e0`: 6→base6x74, 7→base6x7, else→base6x76)

## Goals

- Native Linux and modern Windows (no compatibility layers)
- Faithful to the original — not a reimagining
- Clean, readable C that documents what we learned from reversing
- Original assets required; open-source asset repo support planned

## Non-Goals (for now)

- Multiplayer
- Full editor / asset pipeline tooling

## Building

```bash
# Dependencies (Arch Linux)
sudo pacman -S sdl2 vulkan-icd-loader vulkan-headers glslang cmake lzo

# Dependencies (Ubuntu/Debian)
sudo apt install libsdl2-dev libvulkan-dev glslc cmake build-essential liblzo2-dev

# Build
mkdir build && cd build
cmake ..
make

# Run from project root (game assets must be alongside the binary)
./i76

# Optional: point at your asset directory
./i76 --assets "/path/to/Interstate 76 Nitro Pack"
```

## Project Structure

```
i76/
├── src/
│   ├── main.c                  # Entry point — mirrors Fun_Real_Entry (0x00431760)
│   ├── platform/
│   │   ├── platform.h          # Platform HAL (window, events, timing, valloc)
│   │   └── platform_sdl.c      # SDL2 implementation
│   ├── engine/
│   │   ├── cmdline.c/h         # Command-line parser
│   │   ├── font.c/h            # Bitmap font loader (.fnt format, draws into 8-bit buffer)
│   │   ├── fs.c/h              # Asset root + case-insensitive file open
│   │   ├── gamestate.c/h       # Game state enum and globals
│   │   ├── loop.c/h            # Gameplay frame loop (fixed timestep + interpolated render)
│   │   ├── input.c/h           # Per-frame input snapshot (held/pressed/quit)
│   │   ├── world.c/h           # Simulated world seam (entity store goes here)
│   │   ├── meshcache.c/h       # GEO mesh cache — dual index, refcount, LRU eviction
│   │   ├── geomesh.c/h         # OEG mesh decode — verts + face index lists
│   │   ├── meshview.c/h        # In-engine mesh viewer (--meshview): software wireframe
│   │   ├── pcx.c/h             # PCX image loader (8-bit palettised)
│   │   ├── strlookup.c/h       # StrLookup stub (reimplements strlkup.dll API)
│   │   ├── video.c/h           # Smacker video playback
│   │   ├── zfs.c/h             # ZFS archive reader + LZO decompressor
│   │   └── vfs.c/h             # Virtual filesystem (.zix parser, source routing)
│   └── render/
│       ├── render.h            # Renderer interface
│       ├── render_vk.c         # Vulkan backend (replaces NITSHELL.DLL)
│       └── shaders/
│           ├── blit.vert       # Fullscreen triangle
│           └── blit.frag       # 8-bit index → palette lookup
├── tools/
│   ├── asset_browser.c         # GUI asset browser — file list + PCX/WAV preview (SDL2_ttf)
│   ├── zfs_dump.c              # Extract / list nitro.zfs
│   └── smk_dump.c              # Inspect Smacker .smk files
├── lib/
│   └── libsmacker/             # Smacker decoder (third-party)
├── docs/
│   └── REVERSING.md            # Full reversing notes — structs, functions, globals
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

```bash
# Browse decoded OEG meshes in-engine (rotating wireframe; LEFT/RIGHT cycle, ESC quit)
./i76 --meshview
```

## Credit
Reach out if you feel like you deserve some credit, this is a deep community and I don't know who is behind many of the resources.

"That Tony" did some excellent work deserialising the game binaries, ZFS and VFS systems, much inspiration came from his work. (2007??)

David Hopkinson  aka "Hopper" - provided a great insight with his "Hopper's Guide" which delves deep into the model and texture mapping side of things. (1999??)

## Reversing Notes

See [`docs/REVERSING.md`](docs/REVERSING.md) for everything deduced from the original
binary. Covers: entry points, game state machine, ZFS archive format, VFS layer +
asset load chain (`vfs_lod` → PAK/cached → file cache), the GEO mesh cache + OEG mesh
format, the mission file format and object placement chain (mission → ODEF → OBJ →
entity creator), the chunk/IFF reader, the object→model registry, the PIX/PAK system,
renderer abstraction (NITSHELL.DLL), font system, audio, and all known globals and
function addresses.

## Original Binary

- **Game**: Interstate '76 Nitro Pack (Activision, 1997)
- **Platform**: Win32 x86, MSVC-compiled
- **Reversing tool**: Ghidra
