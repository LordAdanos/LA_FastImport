# In-game test LA_FastImport

Needs: a Gothic II installation (Steam 2.6 fix) with Union that you may start, a GPU, and a world to compile.
Use a copy of the game folder, never your only installation.

## 1. Install

Copy `release\LA_FastImport.dll` and `release\LA_FastImport.ini` to `<Gothic II folder>\System\Autorun\`.

For the test set `Diag=1` in the copied ini (log `System\Autorun\LA_FastImport.log`).
Optional run with `SelfCheck=1` (slower: every reused BSP cache is compared with a full rebuild).
Uninstall: delete both files (and the log).

## 2. Compile run

Compile your tiled world with the engine's world compiler as usual (for example a start with `-zlog:5,s -zwindow`
and your own compile switch). Comparison run without the plugin: `Enabled=0` in the ini (same procedure).

## 3. What the test must check

1. `LA_FastImport.log` line 1: `LA_FastImport v1.2 loaded ... code fingerprint 0x2D47E1AB (expected 0x2D47E1AB).`
   and line 2 `active. ... AllowBigBsp=1` – otherwise the plugin did nothing (other exe build or another patch).
2. zSpy log timing of `BSP: Adding Polys To Bsp-Mesh` (with the plugin: seconds) and of the later phases
   (`Building real BSP`, `zCBspTree::Build`, `GenerateStaticWorldLighting`, SaveWorld) – not changed by the plugin.
3. Log: one pair `MergeMesh #n: ... cache reused` / `MergeMesh #n done` per level mesh (only #1 `rebuilt`) and a
   `LA_FastImport summary` line at the end (now also written at process exit). If lines were lost, the next line
   says `(N log lines lost before)`.
4. No fault dialog "BSP: SAVEBIN: problem" when saving the >90 MB world, and none "BSP: LOADBIN: problem"
   when the game loads it; the world loads completely.
5. Result as before: your compile log reports the world as saved; the world loads, collision and lighting look like before.
6. Optional equality check: compile once with `Enabled=0` (slow, overnight) and compare the two
   resulting world files (MeshAndBsp chunk) byte by byte – expected identical.
