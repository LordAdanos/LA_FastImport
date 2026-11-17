# Changelog

## v1.2 – 2026-11-16

First public release.

- **FastVertexCache**: the engine's sorted vertex merge cache (`zCMesh::s_vertexCache`) is kept as a blocked list with
  the identical logical order, so inserting no longer moves the whole array tail.
- **ReuseMergeCache**: `zCMesh::MergeMesh` keeps the BSP-mesh cache from one call to the next and only inserts the new
  vertices (full rebuild if the vertex list changed).
- **FastMergeRemap**: pointer → first index hash table instead of a linear scan for the source vertex lookup.
- **AllowBigBsp** (default 1): no fault dialog "BSP: SAVEBIN: problem" / "BSP: LOADBIN: problem" for compiled worlds whose
  MeshAndBsp chunk exceeds 90,000,000 bytes (`zCBspTree::SaveBIN` `0x00538927`, `LoadBIN` `0x00538F12`: `jbe` → `jmp` to
  the same target). The reader handles large chunks (field-wise reads, 32-bit sizes), see `doc\ANALYSE.md` §5.
- Log file opened once at DLL load and kept open; lost lines are counted; the summary is also written at process exit.
- Code fingerprint over all engine code the plugin relies on: `0x2D47E1AB` (Steam `Gothic2.exe`, 9,038,140 bytes).

Tests (no game start, `test\run_tests.ps1`): `patchcheck` 13 sites original → patched → restored; 3DS import of 19 level
files identical, 73.3 s → 3.1 s; BSP merge of the first 110 of 274 meshes identical, 1542.7 s → 2.5 s, all 274 in
10.3 s; `SelfCheck` 273/273; `vobload` 1196 vob 3DS files identical with 0 pool errors. In game a full world compile
took about 15 minutes instead of hours.

| File | Bytes | SHA-256 |
|---|---|---|
| `release\LA_FastImport.dll` | 137216 | `F006F34839E622756CD02B8E87EC8F8D346FDE95DD0EDE64C9610055BB0D3DC2` |
| `release\LA_FastImport.ini` | 2357 | `17F2AF98CEB669CA69F8428C8642DBEF95C659D046989D537002F17543542289` |
