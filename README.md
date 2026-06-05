# Interstate '76 Nitro Pack — Open Source Rewrite

A from-scratch reimplementation of Interstate '76 Nitro Pack (Activision, 1997)
for Linux and modern Windows, using the original binary as a reference via Ghidra
decompilation. Requires original game assets, I'm using the GOG nitro version.

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
| Game state machine | Stub — states defined, handlers are empty |
| Audio | Not started |
| Terrain / world | Not started |
| Vehicles / physics | Not started |
| AI / combat | Not started |
| Multiplayer | Not started |

## Immediate TODO

1. Reverse NITSHELL.DLL callback table (32 entries at `&local_80` in `game_session_run`) — needed to write our own ShellMain replacement
2. Stub our own ShellMain that returns `shell_result=2` with a hardcoded scenario name, so we can reach the gameplay state
3. Wire font selection by video mode (`DAT_005fb0e0`: 6→base6x74, 7→base6x7, else→base6x76)
4. Reverse `FUN_0046ea40`, `FUN_0049ae20`, `FUN_00499a60` (early-init unknowns, dual call-site ones are likely reset functions)

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
# List all files in nitro.zfs
./tools/zfs_dump list "/path/to/nitro.zfs"

# Extract entire archive
./tools/zfs_dump extract "/path/to/nitro.zfs" out/

# Extract one file
./tools/zfs_dump get "/path/to/nitro.zfs" FILENAME.EXT out.bin
```

## Reversing Notes

See [`docs/REVERSING.md`](docs/REVERSING.md) for everything deduced from the original
binary. Covers: entry points, game state machine, ZFS archive format, VFS layer,
renderer abstraction (NITSHELL.DLL), font system, audio, and all known globals and
function addresses.

## Original Binary

- **Game**: Interstate '76 Nitro Pack (Activision, 1997)
- **Platform**: Win32 x86, MSVC-compiled
- **Reversing tool**: Ghidra
