# Addresses

Gothic 2 NotR 2.6 fix, Steam `Gothic2.exe`: 9,038,140 bytes, ImageBase `0x00400000`.
Source: static analysis (disassembly) of this exe, checked by running the
engine code in the test harness (`test\harness.cpp`).

## Exe check

FNV-1a fingerprint over all code ranges the plugin relies on (`kCodeRanges` in `src/la_fastimport.cpp`),
expected value `0x2D47E1AB`. On a different value the plugin patches nothing. Before writing, every patch
site is compared with the original bytes below, and the three call sites must load `ecx = 0x008D87B8`
(`B9 B8 87 8D 00`) right before the call.

| Range | Function |
|---|---|
| `0x005680A0`–`0x0056831A` | `zCMesh::S_InitVertexMergeCache(zCMesh*)` (static, cdecl) |
| `0x00568320`–`0x00568345` | `zCMesh::S_DeleteVertexMergeCache()` |
| `0x00568350`–`0x005684D8` | `zCMesh::VertexInMesh(const zVEC3&)` |
| `0x005686B0`–`0x005688EA` | `zCMesh::AddVertex(zCVertex*)` |
| `0x005688F0`–`0x00568B71` | `zCMesh::AddVertex(zVEC3&)` |
| `0x0056A9B0`–`0x0056AE8B` | `zCMesh::MergeMesh(zCMesh*, const zMAT4&)` |
| `0x00570CB6`–`0x00570CCD` | `zCMesh::LoadMSH`, InsertSort call |
| `0x00572AC0`–`0x00572CE3` | vertex compare (`|dx|,|dy|,|dz| < 0.01` → 0, else x, y, z order) |
| `0x00575830`–`0x00575975` | `zCArraySort<zCVertex*>::InsertSort` |
| `0x00575980`–`0x0057599A` | `zCArraySort<zCVertex*>::DeleteList` |
| `0x00565F80`–`0x00565F85` | `operator delete[]` |
| `0x00538900`–`0x00538A60` | `zCBspTree::SaveBIN` part (function `0x00538010`, called by `zCWorld::Archive`/`SaveBspTree`): chunk length check |
| `0x00538EE0`–`0x00539000` | `zCBspTree::LoadBIN` (`0x00538E10`) part: chunk length check |

## Patched

| Address | Original bytes | New | What |
|---|---|---|---|
| `0x00568173` | `8B 15 B8 87 8D 00` | `jmp StubInit` + nop | S_InitVertexMergeCache after the log block: our list is filled with all mesh vertices; back to `0x005682FD` (epilogue sets `s_usesVertexCache = 1`) |
| `0x00568320` | `A1 B8 87 8D 00` | `jmp HookDeleteCache` | S_DeleteVertexMergeCache (whole function) |
| `0x005683E0` | `8B 54 24 14 8B 02` | `jmp StubFind` + nop | VertexInMesh, cache branch after the static key guard; back to `0x005684D3` (`pop`s, `ret 4`, eax = result) |
| `0x0056880E` | `8B 0D C0 87 8D 00` | `jmp StubAddPtr` + nop | AddVertex(zCVertex*): cache insert; back to `0x005688D4` |
| `0x00568A19` | `8B 53 38 8B 43 40` | `jmp StubAddVec` + nop | AddVertex(zVEC3&): cache insert of `verts[numVert]`; back to `0x00568B5B` |
| `0x0056AB31` | `B9 B8 87 8D 00 E8 45 AE 00 00` | `jmp StubMergeRebuild` + nops | MergeMesh: `DeleteList`, compare = `0x00572AC0`, `InsertSort` of every BSP-mesh vertex; back to `0x0056AB69` |
| `0x0056ACDE` | `E8 4D AB 00 00` | `call HookInsertSort` | MergeMesh: InsertSort of a new vertex |
| `0x00570CC8` | `E8 63 4B 00 00` | `call HookInsertSort` | LoadMSH: InsertSort |
| `0x0056AE61` | `E8 1A AB 00 00` | `call HookMergeDeleteList` | MergeMesh end: DeleteList (keeps the list for the next MergeMesh) |
| `0x0056AB77` | `8B 46 38 C1 E0 02` | `jmp StubMergeMap` + nop | MergeMesh: builds the pointer→index table of the source mesh, re-executes the two instructions; back to `0x0056AB7D` |
| `0x0056ADF1` | `8B 56 38 33 C0` | `jmp StubMergeRemap` | MergeMesh: polygon vertex lookup in the source vertex list; back to `0x0056AE1A` |

| `0x00538927` | `0F 86 23 01 00 00` | `E9 24 01 00 00 90` (jmp `0x00538A50`) | SaveBIN: `cmp eax,90000000` / `jbe` → always the "size ok" path (no fault report "BSP: SAVEBIN: problem") |
| `0x00538F12` | `0F 86 E1 00 00 00` | `E9 E2 00 00 00 90` (jmp `0x00538FF9`) | LoadBIN: same for "BSP: LOADBIN: problem"; `edi` (chunk end) is already set at `0x00538F0E`, exactly as on the original small-chunk path |

Groups and ini switches: `FastVertexCache` = the first nine rows, `FastMergeRemap` = the next two, `AllowBigBsp` = the two BIN rows.
The constant 90,000,000 (`80 4A 5D 05`) occurs only at these two places in the exe (byte scan).
`ReuseMergeCache` is a mode of the MergeMesh hooks.

## Data

| Address | What |
|---|---|
| `0x008D87B8` | `zCMesh::s_vertexCache` (`zCArraySort<zCVertex*>`): array pointer; `+4` numAlloc, `+8` numInArray, `+0xC` compare function. Stays empty while the plugin is active. |
| `0x008D886C` | `zCMesh::s_usesVertexCache` |
| `0x008D8870` | `zCMesh::s_bAddVobsToMesh` (MergeMesh skips the cache when set) |
| `0x008D884C` | static `zVEC3` search key of VertexInMesh (written like the engine does) |
| `0x008CDCF0` | zerr log level (byte) |

## Members

| Offset | What |
|---|---|
| `zCMesh +0x34` | numPoly |
| `zCMesh +0x38` | numVert |
| `zCMesh +0x40` | `zCVertex**` vertex list |
| `zCMesh +0x44` | `zCPolygon**` polygon list |
| `zCMesh +0xDC / +0xE0` | allocated vertices / polygons |
| `zCPolygon +0x00` | `zCVertex**` vertices, `+0x08` plane (d, normal), `+0x18` material, `+0x30` vertex count (byte) |
| `zCVertex +0x00` | position (3 floats) |

## Read only (analysis, not patched)

| Address | What |
|---|---|
| `0x0055D660` | `zCFile3DS::Load3DS`: `S_InitVertexMergeCache` (unless `-ZLOAD3DSOLDMETHOD`), chunk reader `0x0055CBC0`, `FlushMeshBuffer` `0x0055D090`, `S_DeleteVertexMergeCache`, `CalcBBox3D`, `CalcVertexNormals(2, NULL)` (mode 2 = flat, linear), `PrelightMesh` |
| `0x0055D090` | FlushMeshBuffer: `AllocVerts`, `AddVertexSmart` per buffer vertex, `AllocPolys`, `AddPoly(v,v,v)` `0x00568EA0` per face |
| `0x00568B80` | `zCMesh::AddVertexSmart` = `VertexInMesh` + `AddVertex(zVEC3&)` |
| `0x0053D310` | `zCCBspTree::AddMesh` ("BSP: Adding Polys To Bsp-Mesh") = `MergeMesh` into the BSP mesh |
| `0x00622EC0` | `zCWorld::CompileWorld` (OUTDOOR: `MarkGhostOccluder`, `zCCBspTree::BuildTree`, `zCBspTree::Build`) |
| `0x006C9A50` | `oCGame::CompileWorld`: `-OUTDOOR` → mode, then `GenerateStaticWorldLighting` |
| `0x00569DE0` | `zCMesh::CalcVertexNormals`: modes 0/1 without BSP tree scan all polygons per vertex (O(P²)); with BSP tree (world lighting) a box query; mode 2 linear |
| `0x0053D610` / `0x00540210` | `zCCBspTree::BuildTree` / `zCCBspNode::BuildTree` (octree split, O(P · depth)) |
