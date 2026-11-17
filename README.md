# LA_FastImport

Union plugin for **Gothic 2: Night of the Raven 2.6 fix** that removes the quadratic costs of the engine's 3DS import and
of the BSP compile of large worlds (identical results, bit for bit), and suppresses the false "BSP: SAVEBIN/LOADBIN:
problem" dialog for compiled worlds over 90 MB.

Version: see `CHANGELOG.md`.

What the engine does (see `doc/ANALYSE.md`):

1. `zCMesh::s_vertexCache` is one sorted array; every new vertex is inserted with `memmove` of the array tail
   (3DS import via `AddVertexSmart`, and the BSP merge). Cost O(n²) per file / per merged mesh.
2. `zCMesh::MergeMesh` (called by `zCCBspTree::AddMesh` for every level mesh) rebuilds that cache from all vertices
   of the growing BSP mesh on each call: O(V_total²) per call.
3. `zCMesh::MergeMesh` looks up every polygon vertex in the source vertex list with a linear scan: O(P·V) per call.
4. `zCBspTree::SaveBIN`/`LoadBIN` report a fault when the MeshAndBsp chunk exceeds 90,000,000 bytes, although the
   data is written and read field by field with 32-bit sizes.

What the plugin does:

1. Keeps the identical logical sorted array in a blocked list (same binary search index sequence, same engine compare
   function on the same elements) — insert without moving the whole array.
2. Keeps the BSP-mesh cache from one `MergeMesh` to the next and only inserts the new vertices, after checking that the
   BSP mesh's vertex list still has the same pointers and bit-identical positions; otherwise full rebuild.
3. Uses a pointer → first index hash table for the source vertex lookup.
4. Turns the two `jbe` of the 90 MB checks into `jmp` to the same target (`AllowBigBsp=1`).

Install: copy `release/LA_FastImport.dll` and `release/LA_FastImport.ini` to `Gothic II\System\Autorun\` (needs Union).
The plugin verifies an FNV-1a fingerprint over all engine code it relies on (Steam `Gothic2.exe`, 9,038,140 bytes,
fingerprint `0x2D47E1AB`) and the original bytes of every patch site; on any mismatch it patches nothing.

## Build and tests

`build.bat` (Visual Studio 2019 or newer, C++ x86 tools; newest installed one is used) builds `build\LA_FastImport.dll`
(32-bit, static CRT) and `build\lafi_harness.exe`; `build.bat dll` builds the DLL only. `test\run_tests.ps1 -GameDir <Gothic II folder>` runs the engine's own machine code from `Gothic2.exe` (mapped read-only, game never started), once
original and once patched, and compares the results bit for bit (`-LevelDir`, `-ZenFile`, `-VobList` select inputs).

## Who is this for / limitations

- Players of mods with very large worlds: if a compiled world has a MeshAndBsp chunk above 90 MB, the engine shows the
  fault dialog "BSP: LOADBIN: problem" on every world load (`zCBspTree::LoadBIN`, independent of the renderer).
  `AllowBigBsp` removes that dialog; the data loads correctly either way. With fullscreen renderers the dialog may end up
  behind the game window and look like a freeze (not verified).
- Modders who compile worlds or import 3DS files in the game itself get the speed-up (BSP merge 1543 s -> 2.5 s, 3DS
  import 73 s -> 3 s, measured).
- Only the Steam `Gothic2.exe` 2.6 fix is supported (exe size and code fingerprint check). The Spacer is a different
  executable and is NOT supported or tested.
- Spacer.NET (Union spacer) uses its own import/merge code, so LA_FastImport gives no speed-up there; it only helps when
  worlds are compiled or 3DS-imported by the engine itself (e.g. in-game compile). Spacer.NET hooks the entry of
  `zCMesh::MergeMesh` inside the fingerprinted range, so if it is loaded first, LA_FastImport switches off completely,
  including `AllowBigBsp` (source analysis of Spacer.NET 1.58, not tested live).

## Requirements

- Gothic 2 Night of the Raven 2.6 fix (Steam `Gothic2.exe`, 9,038,140 bytes), 32-bit.
- **Union 1.0m** (plugin host that loads the DLL from `System\Autorun`). SystemPack 1.8 (shipped with Union) is compatible.
- No Visual C++ Redistributable needed: the DLL is built with `/MT` (statically linked CRT, see `build.bat`).
  Visual Studio is only needed to build it yourself.

Tested with: Steam Gothic 2 NotR 2.6 fix + Union 1.0m + SystemPack 1.8, with Ikarus/LeGo, in a large custom world with many vobs, and together with a renderer replacement

## Install / uninstall

Copy `release\LA_FastImport.dll` and `release\LA_FastImport.ini` to `<Gothic II>\System\Autorun\`. Uninstall: delete both
files (and `LA_FastImport.log`).

## INI switches (`LA_FastImport.ini`, section `[LA_FASTIMPORT]`)

`Enabled`, `FastVertexCache`, `ReuseMergeCache`, `FastMergeRemap`, `AllowBigBsp`, `SelfCheck`, `Diag` (all 0/1, see the
comments in the ini). `Enabled=0` runs Gothic exactly as without the plugin.

## Log

With `Diag=1`: `LA_FastImport.log` next to the DLL (load line with the code fingerprint, patch result, per-`MergeMesh`
lines, summary at exit).

## Support

Please open a GitHub Issue and attach `LA_FastImport.log`.

Developed with AI assistance; tested by the author.

## Files

| File | Content |
|---|---|
| `src/la_fastimport.cpp` | the whole plugin |
| `src/la_exe_image.h` | test-harness loader (maps `Gothic2.exe`) |
| `release/LA_FastImport.dll`, `release/LA_FastImport.ini` | ready-to-use version (hashes in `CHANGELOG.md`) |
| `CHANGELOG.md` | versions, test results, SHA-256 of the release files |
| `doc/ADDRESSES.md` | patched addresses, original bytes, checked code ranges |
| `doc/ANALYSE.md` | analysis: hot spots, complexity, why the results are identical, measurements |
| `doc/INGAME_TEST.md` | in-game test (install, compile command, what to check) |
| `test/harness.cpp` | test program: runs the engine's own code original vs. patched (loader: `src/la_exe_image.h`) |
| `test/run_tests.ps1` | runs the harness tests (`-GameDir` mandatory), logs to `test\out\` |

## Credits and license

Own code, MIT license (`LICENSE`). Architecture (single source, Autorun DLL, code check, ini, log) follows the
author's plugin [VobWay](https://github.com/LordAdanos/VobWay) (MIT); no code was copied from it. No other third-party code.
