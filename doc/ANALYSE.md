# LA_FastImport – analysis: 3DS import and BSP build in Gothic2.exe 2.6 fix

Source: static analysis (disassembly) of the Steam `Gothic2.exe` (9,038,140 bytes), then the original machine code run in
the test program `test\harness.cpp`. Addresses: `ADDRESSES.md`. This document gives addresses and descriptions in our own
words; it contains no code from the exe.

## 1. Flow

**3DS import** `zCFile3DS::Load3DS` (0x0055D660):
`S_InitVertexMergeCache(mesh)` → chunk reader (0x0055CBC0) → `FlushMeshBuffer` (0x0055D090: `AllocVerts`,
`AddVertexSmart` = `VertexInMesh` + `AddVertex` for every buffer vertex, `AllocPolys`, `AddPoly(v,v,v)` per face)
→ `S_DeleteVertexMergeCache` → `CalcBBox3D` → `CalcVertexNormals(2, NULL)` → `PrelightMesh`.

**World compile** `oCGame::CompileWorld` (0x006C9A50) → `zCWorld::CompileWorld` (0x00622EC0, with `-OUTDOOR`):
`TraverseBsp` → for every level mesh `zCCBspTree::AddMesh` (0x0053D310, log "BSP: Adding Polys To Bsp-Mesh")
= `zCMesh::MergeMesh(bspMesh, levelMesh, trafo)` (0x0056A9B0) → `MarkGhostOccluder` → `zCCBspTree::BuildTree`
→ `zCBspTree::Build` → then `GenerateStaticWorldLighting` (`CalcVertexNormals(1, bspTree)`, vertex light).

## 2. Hot spots

| # | Location | What | Cost |
|---|---|---|---|
| H1 | Vertex merge cache `zCMesh::s_vertexCache` (0x008D87B8), insert in `AddVertex` (0x005686B0, 0x005688F0), `S_InitVertexMergeCache` (0x005680A0), `zCArraySort<zCVertex*>::InsertSort` (0x00575830) | One sorted array; binary search, then `memmove` of the whole tail for every new vertex | O(V) per new vertex → **O(V²) per 3DS file** |
| H2 | `zCMesh::MergeMesh` 0x0056AB31..0x0056AB69 | On **every** call (= every level mesh / tile) `DeleteList` and a rebuild of the cache from **all** vertices of the growing BSP mesh via `InsertSort` (memmove) | O(V_bsp²) per tile (unsorted order), ~O(K·V²) overall |
| H3 | `zCMesh::MergeMesh` 0x0056ADE1..0x0056AE1A | Every polygon vertex of the source is searched linearly in the source vertex list | O(P·V) per tile |
| – | `zCMesh::CalcVertexNormals` (0x00569DE0) | Modes 0/1 **without** a BSP tree: all polygons for every polygon vertex (O(P²)). The 3DS import only uses mode 2 (flat, linear); world light uses the BSP tree (box query). | **not** a hot spot on this path |
| – | `zCCBspNode::BuildTree` (0x00540210), `SelectPlaneOctree` | octree split, one pass over the polygons per level | O(P·depth), unremarkable |
| – | `zCCBspNode::OutdoorKillRedundantLeafs`, `PushPolyFront` | linear list search only inside one node | small |
| ? | `zCBspTree::Build` (`CreateBspSectors2`, `MarkOccluderPolys`, `OptimizeLight`), `GenerateStaticWorldLighting` | not evaluated in detail | measure in game |

H1 explains the quadratic 3DS import, H2+H3 the ever slower "Adding Polys To Bsp-Mesh": the cost depends on the BSP
mesh built so far, and houses (many parts, vertex order not sorted by x → large memmoves) are the most expensive.

Compare function 0x00572AC0: equal if |dx|, |dy|, |dz| < 0.01; otherwise ordered by x, then y, then z. Because of the
tolerance this is not a strict order – the result of the binary search depends on the exact index sequence and on the
array contents. A hash merge ("same position → same vertex") would therefore merge **different** vertices than the
original. That is why the plugin replaces only the container, not the search.

## 3. Replacement and proof of equality

**R1 (H1): same logical sequence, different container.** The cache contents live in a blocked list (blocks ≤ 1024
entries, start index per block). Insert and search use exactly the engine's index arithmetic (`lo/hi/mid`, truncating
division like `cdq/sub/sar`, the same comparison before/after the loop) and call the engine compare function (pointer at
0x008D87C4) with the same elements. Induction over the operations: same sequence before + same comparison results ⇒ same
insert position or same hit ⇒ same sequence after. Every place that reads or writes the array is replaced
(cross-references to 0x008D87B8..C4 and `s_usesVertexCache`): init, delete, `VertexInMesh`, both `AddVertex`, the
`InsertSort` calls in `MergeMesh` and `LoadMSH`, `DeleteList` in `MergeMesh`. The engine array stays empty.

**R2 (H2): cache reuse.** The rebuild in `MergeMesh` is "insert v0..vn-1 of the BSP mesh list in order". At the end of
the previous `MergeMesh` the cache holds exactly "v0..vm-1 inserted in order" (rebuild + new vertices in append order).
If the first m entries of the list are unchanged at the next call (same pointers, bit-identical positions – checked,
O(m)), continuing with vm..vn-1 gives the same sequence as the rebuild (inserting is a left fold). Otherwise a full
rebuild. `SelfCheck=1` additionally compares every reuse with a full rebuild.

**R3 (H3): hash instead of scan.** Table pointer → **first** index (like the forward scan); a hit is verified against
the list, otherwise fall back to the scan.

Floating point: the plugin computes nothing in floating point; transformation, normals and comparisons stay engine code.

## 4. Measurements in the test program

Test program `build\lafi_harness.exe` (read-only: `Gothic2.exe` and the level 3DS files): loads the exe at 0x400000,
resolves imports (system DLLs from System32, SmartHeap → own heap, game DLLs such as ddraw/mss32/vdfs32g → stubs that
are never loaded), runs `_heap_init`, `_mtinit`, `_ioinit` and all 1780 static initializers (all ok) and then calls the
engine functions directly. Every test runs original and with the plugin; compared are vertex positions (bits), vertex
assignment, polygon vertex indices and polygon planes.

Logs: `test_*.log` in the output folder of `test\run_tests.ps1` (one core, no game start, no GPU). Inputs: the level
files of a large tiled mod world.

| Test | Scope | Original | With plugin | Result |
|---|---|---|---|---|
| Import, untiled level 3DS | 19 files, 3.66 M faces | 73.3 s (terrain mesh alone 60.8 s, 883 k vertices) | 3.1 s (terrain 1.6 s) | **identical**, 24× (terrain 37×) |
| Import, tiles (≤ 17 k faces) | 268 files | 3.1 s | 2.2 s | **identical**, 1.4× (tiles are already small) |
| BSP merge (`MergeMesh`, = "Adding Polys To Bsp-Mesh"), tiles in ZEN order | 274 meshes, 3.66 M faces | first 110 meshes (722 k vertices, 1.44 M polygons): **1542.7 s**, at the end 46 s per tile and growing quadratically | first 110: **2.5 s**; all 274 (1.77 M vertices, 3.5 M polygons): **10.3 s** | **identical** (vertices, polygon vertex indices, polygon planes bit-identical; hash `9A8D1D1DACC33053`), 608× |
| Merge with `SelfCheck` (every reuse against a full rebuild) | 274 meshes | – | 301.9 s (273 self checks; a full rebuild with the blocked list costs ~1 s at 1.7 M vertices) | **273 of 273 reuses = full rebuild**, 0 errors |

Extrapolation of the original for all 274 meshes: time per tile grows ~quadratically with the BSP vertices (46 s at
0.7 M → ~290 s at 1.77 M) → roughly **6–8 hours** for "Adding Polys To Bsp-Mesh" alone. With the plugin: ~10 s.

Limits of this statement: the test program calls `MergeMesh` with an identity matrix and polygons without material
(material is only copied/reference-counted) and builds the tile meshes directly (without the zCBinTree3DS pre-dedup of
the 3DS reader). BSP tree, world light and SaveWorld did not run – that is checked by the in-game test
(`INGAME_TEST.md`).

## 5. 90 MB limit in SaveBIN/LoadBIN

A compiled world with a 212 MB MeshAndBsp chunk triggers "D: BSP: SAVEBIN: problem … chunkLen: 212484755" when saving
and "BSP: LOADBIN: problem" when loading (fault dialog; after OK the game continues).

- **Locations:** `zCBspTree::SaveBIN` (0x00538010, callers `zCWorld::Archive`, `zCWorld::SaveBspTree`): after writing,
  the chunk length is patched in, then `cmp eax,90000000` / `jbe 0x538A50` (0x0053891E..0x00538927), otherwise
  `kZErrorReport(…, 3 = Fault, …)`. `zCBspTree::LoadBIN` (0x00538E10): read the chunk length,
  `edi = position + length`, `cmp eax,90000000` / `jbe 0x538FF9` (0x00538F03..0x00538F12), otherwise the same fault.
  The constant occurs in the exe only at these two places (byte search for `80 4A 5D 05`).
- **Effect:** in both cases only a message. Afterwards the same code runs as for small chunks (save: nothing more;
  load: `edi` is restored from `[esp+24h]` after the message, i.e. the same value as on the short path).
- **Can the reader handle > 90 MB?** Yes: `LoadBIN` reads field by field through the virtual read method of the `zFILE`
  (`+0xCC`), sizes are 32-bit `int` (polygons, nodes, leaves, sectors each in their own `operator new` arrays with
  `count * size`), no buffer of chunk size; the mesh comes field by field through `zCMesh::LoadMSH` as well. The limit is
  2 GB (signed int) or the address space (Gothic is LARGEADDRESSAWARE). In game a 212 MB world was saved after the OK
  and loads completely.
- **Patch (`AllowBigBsp=1`):** `jbe` → `jmp` to the same target (6 bytes, original bytes checked, ranges in the
  fingerprint). Harness `patchcheck`: after patching both jumps point exactly to the target of the original `jbe`;
  `Remove` restores all 13 sites.

## 6. Log file

The log file is opened once at DLL load (`CreateFileW`, `FILE_APPEND_DATA`, full sharing) and kept open; lines are
written with `WriteFile`. Opening the file per line can fail inside the running game (Union hooks the file functions),
which silently lost lines. If a write fails, it is counted and the next line reports "(N log lines lost before)". The
summary is also written at process exit (`ExitGame` → `DLL_PROCESS_DETACH` with `reserved != NULL`), not only when the
DLL is unloaded.

## 7. zCVertex pool crash during world loading: not caused by this plugin

A crash in `zCMemPoolBase::Alloc` (0x00566649) while loading vob visuals (`AddVertexSmart` ← `FlushMeshBuffer` ←
`Load3DS` ← `zCProgMeshProto::Load` ← `zCVob::SetVisual`) can look related, because the plugin replaces the vertex merge
cache on that path. It is not:

- The plugin never writes into vertices; find/insert only read pointers. The cache is filled only between init and
  delete (`Load3DS` 0x0055D86F/0x0055D8CD, `zCProgMeshProto::GetMesh`) and empty afterwards (harness: size 0, engine array
  NULL, `s_usesVertexCache` 0). No threading.
- Harness `vobload`: 1196 vob 3DS files in load order, real zCMesh objects (`_CreateNewInstance`), import like
  `FlushMeshBuffer`, `CalcBBox3D`, `zCMesh::DeleteAll` (all vertices back into the pool, the next file reuses the free
  slots), free-list check of the vertex pool after every file: original **0 errors**, with plugin **0 errors**, result
  per file **identical** (1196/1196). The vob import is equally fast with and without the plugin (36.0 s / 35.1 s).

The actual cause is a double free of degenerate polygons in `zCMesh::DeletePolyAndVerts` during model conversion
(`-zautoconvertdata`); it is fixed by the separate plugin LA_AlphaPolyLimit (`DegenerateFix`).
