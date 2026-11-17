// LA_FastImport v1.2 - Union plugin for Gothic 2 NotR 2.6 fix
// License: MIT (see README.md). Own code; architecture after the author's plugin VobWay
// (https://github.com/LordAdanos/VobWay, MIT): single source, 32-bit DLL with static runtime,
// loaded from System\Autorun by Union, code check against the Steam Gothic2.exe before patching,
// ini switches, optional log. No code was copied from VobWay.
//
// What it speeds up (addresses: ADDRESSES.md, analysis: ANALYSE.md):
//  1. zCMesh vertex merge cache (s_vertexCache, a zCArraySort<zCVertex*> at 0x008D87B8).
//     The engine keeps it as one sorted array and inserts with memmove: O(n) per new vertex,
//     O(n^2) per 3DS file (zCFile3DS::Load3DS -> AddVertexSmart) and per BSP mesh merge.
//     We keep the SAME logical array (same binary search, same index arithmetic, the engine's
//     own compare function 0x00572AC0 called on the same elements) in a blocked list:
//     insert O(sqrt n)-ish instead of O(n). Results are identical by construction.
//  2. zCMesh::MergeMesh (zCCBspTree::AddMesh, "BSP: Adding Polys To Bsp-Mesh") rebuilds the whole
//     cache from all vertices of the growing BSP mesh for every merged mesh: O(V_total^2) per call.
//     The rebuilt cache equals the cache left by the previous merge if the BSP mesh's vertex list
//     is unchanged (same pointers, bit-identical positions) -> we keep it and only insert the new
//     tail. Verified on every call; on any difference the full rebuild runs (still with the fast list).
//  4. zCBspTree::SaveBIN (0x538010) / LoadBIN (0x538E10) show a fault dialog when the MeshAndBsp
//     chunk is larger than 90,000,000 bytes (only a message: the data is written/read field by
//     field with 32-bit sizes). AllowBigBsp=1 takes the branch of the "size ok" case instead.
//  3. zCMesh::MergeMesh maps polygon vertices of the source mesh to the new vertices with a
//     linear search over the source vertex list per polygon vertex (O(P*V) per merged mesh).
//     We use a pointer->first-index hash table (same result as the first match of the scan).
//
// The polygon/vertex order, the merge decisions and all floating point values stay exactly as in
// the engine; only containers and searches change. Checked in a harness that runs the exe's own
// machine code (plugins\LA_FastImport\test).

#define LAFI_VERSION "v1.2"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace lafi {

constexpr uint32_t kImageBase = 0x00400000;

// ---- engine addresses (Steam Gothic2.exe 2.6 fix, 9,038,140 bytes) -----------------------------
// vertex merge cache zCArraySort<zCVertex*> zCMesh::s_vertexCache: array, numAlloc, numInArray, compare
constexpr uint32_t kCacheArray = 0x008D87B8;
constexpr uint32_t kCacheAlloc = 0x008D87BC;
constexpr uint32_t kCacheNum = 0x008D87C0;
constexpr uint32_t kCacheCompare = 0x008D87C4;
constexpr uint32_t kUsesVertexCache = 0x008D886C;  // zCMesh::s_usesVertexCache
constexpr uint32_t kCompareVerts = 0x00572AC0;     // the engine's comparator (eps 0.01, then x,y,z)
constexpr uint32_t kOperatorDeleteArr = 0x00565F80; // operator delete[]
constexpr uint32_t kFindKey = 0x008D884C;           // static zVEC3 of VertexInMesh (written like the engine)

// zCMesh members
constexpr uint32_t kMeshNumPoly = 0x34;
constexpr uint32_t kMeshNumVert = 0x38;
constexpr uint32_t kMeshVertList = 0x40;

// patch sites
constexpr uint32_t kInitSite = 0x00568173;      // S_InitVertexMergeCache: after the log block
constexpr uint32_t kInitBack = 0x005682FD;      //   epilogue (sets s_usesVertexCache = 1)
constexpr uint32_t kDeleteSite = 0x00568320;    // S_DeleteVertexMergeCache: whole function
constexpr uint32_t kFindSite = 0x005683E0;      // VertexInMesh: cache branch after the static-key guard
constexpr uint32_t kFindBack = 0x005684D3;      //   pop edi/esi/ebp/ebx; ret 4 (eax = result)
constexpr uint32_t kAddPtrSite = 0x0056880E;    // AddVertex(zCVertex*): cache insert block
constexpr uint32_t kAddPtrBack = 0x005688D4;
constexpr uint32_t kAddVecSite = 0x00568A19;    // AddVertex(zVEC3&): cache insert block
constexpr uint32_t kAddVecBack = 0x00568B5B;
constexpr uint32_t kMergeRebuildSite = 0x0056AB31;  // MergeMesh: DeleteList + compare + InsertSort loop
constexpr uint32_t kMergeRebuildBack = 0x0056AB69;
constexpr uint32_t kMergeInsertCall = 0x0056ACDE;   // MergeMesh: call InsertSort (new vertex)
constexpr uint32_t kLoadMshInsertCall = 0x00570CC8; // LoadMSH: call InsertSort
constexpr uint32_t kMergeDeleteCall = 0x0056AE61;   // MergeMesh: call DeleteList at the end
constexpr uint32_t kMergeMapSite = 0x0056AB77;      // MergeMesh: before the remap array allocation (esi = source mesh)
constexpr uint32_t kMergeMapBack = 0x0056AB7D;
constexpr uint32_t kMergeRemapSite = 0x0056ADF1;    // MergeMesh: linear search of a polygon vertex in the source list
constexpr uint32_t kMergeRemapBack = 0x0056AE1A;
constexpr uint32_t kSaveBinCheck = 0x00538927;      // zCBspTree::SaveBIN: jbe 0x538A50 after cmp eax,90000000
constexpr uint32_t kSaveBinOk = 0x00538A50;
constexpr uint32_t kLoadBinCheck = 0x00538F12;      // zCBspTree::LoadBIN: jbe 0x538FF9 after cmp eax,90000000
constexpr uint32_t kLoadBinOk = 0x00538FF9;

// ---- settings ----------------------------------------------------------------------------------
int g_enabled = 1;
int g_fastCache = 1;    // 1: blocked list for s_vertexCache
int g_reuseMerge = 1;   // 1: keep the BSP-mesh cache between MergeMesh calls (needs FastCache)
int g_fastRemap = 1;    // 1: hash lookup in MergeMesh
int g_diag = 0;
int g_diagMaxLines = 2000;
int g_selfCheck = 0;    // 1: compare every reused cache against a full rebuild (slow, for tests)
int g_allowBigBsp = 1;  // 1: no 90 MB fault dialog in zCBspTree::SaveBIN/LoadBIN

HMODULE g_module = nullptr;
int g_diagLines = 0;
int g_patchedCache = 0;
int g_patchedMerge = 0;
int g_patchedRemap = 0;

// ---- log ---------------------------------------------------------------------------------------
HANDLE g_logFile = INVALID_HANDLE_VALUE;
int g_logDropped = 0;

// The log file is opened once (at DLL load, see Install) and kept open with full sharing:
// opening it again later from inside the running game failed silently in the first in-game run.
bool OpenLog() {
    if (g_logFile != INVALID_HANDLE_VALUE) return true;
    wchar_t path[MAX_PATH];
    if (!g_module || !GetModuleFileNameW(g_module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"LA_FastImport.log");
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_logFile != INVALID_HANDLE_VALUE;
}

void CloseLog() {
    if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
}

void Log(const char* text) {
    if (!g_diag || g_diagLines >= g_diagMaxLines) return;
    ++g_diagLines;
    if (!g_module) {
        OutputDebugStringA(text);
        OutputDebugStringA("\n");
        return;
    }
    if (!OpenLog()) {
        ++g_logDropped;
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[600];
    int n;
    if (g_logDropped) {
        n = sprintf_s(line, "%02d:%02d:%02d.%03d (%d log lines lost before) %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                      g_logDropped, text);
        g_logDropped = 0;
    } else {
        n = sprintf_s(line, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    }
    DWORD written = 0;
    if (n <= 0 || !WriteFile(g_logFile, line, DWORD(n), &written, nullptr)) {
        ++g_logDropped;
        CloseLog();
    }
}

double NowMs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// ---- engine views ------------------------------------------------------------------------------
template <class T> T& Ref(uint32_t address) { return *reinterpret_cast<T*>(address); }
template <class T> T& Member(const void* object, uint32_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(object) + offset);
}
using CompareFn = int(__cdecl*)(const void*, const void*);

inline int MeshNumVerts(const void* mesh) { return Member<int>(mesh, kMeshNumVert); }
inline void** MeshVerts(const void* mesh) { return Member<void**>(mesh, kMeshVertList); }

// Frees the engine's own array (it stays empty while the plugin is active) like DeleteList.
void ClearEngineArray() {
    void* arr = Ref<void*>(kCacheArray);
    if (arr) reinterpret_cast<void(__cdecl*)(void*)>(kOperatorDeleteArr)(arr);
    Ref<void*>(kCacheArray) = nullptr;
    Ref<int>(kCacheAlloc) = 0;
    Ref<int>(kCacheNum) = 0;
}

// ---- the logical sorted array ------------------------------------------------------------------
// Same sequence of elements as the engine's array would hold. Blocks of up to 2*kBlock entries;
// start[b] = logical index of the first element of block b.
class BlockList {
public:
    static constexpr int kBlock = 512;

    int Size() const { return size_; }
    void Clear() {
        blocks_.clear();
        start_.clear();
        size_ = 0;
        lastBlock_ = 0;
    }
    void Swap(BlockList& other) {
        blocks_.swap(other.blocks_);
        start_.swap(other.start_);
        std::swap(size_, other.size_);
        std::swap(lastBlock_, other.lastBlock_);
    }
    // address of element i (0 <= i < size)
    void** At(int i) {
        const int b = Locate(i);
        return &blocks_[b][i - start_[b]];
    }
    void InsertAt(int i, void* value) {
        if (blocks_.empty()) {
            blocks_.emplace_back();
            blocks_.back().reserve(2 * kBlock + 1);
            start_.push_back(0);
        }
        int b = (i >= size_) ? int(blocks_.size()) - 1 : Locate(i);
        std::vector<void*>& block = blocks_[b];
        block.insert(block.begin() + (i - start_[b]), value);
        ++size_;
        for (size_t k = b + 1; k < start_.size(); ++k) ++start_[k];
        if (int(block.size()) > 2 * kBlock) Split(b);
    }
    // copy into a flat array (tests only)
    void Flatten(std::vector<void*>& out) const {
        out.clear();
        out.reserve(size_);
        for (const auto& block : blocks_) out.insert(out.end(), block.begin(), block.end());
    }

private:
    int Locate(int i) {
        // most binary-search probes hit neighbouring blocks: try the last one first
        if (lastBlock_ < int(start_.size()) && start_[lastBlock_] <= i &&
            (lastBlock_ + 1 == int(start_.size()) || i < start_[lastBlock_ + 1])) {
            return lastBlock_;
        }
        const auto it = std::upper_bound(start_.begin(), start_.end(), i);
        lastBlock_ = int(it - start_.begin()) - 1;
        return lastBlock_;
    }
    void Split(int b) {
        std::vector<void*> upper(blocks_[b].begin() + kBlock, blocks_[b].end());
        blocks_[b].resize(kBlock);
        upper.reserve(2 * kBlock + 1);
        const int upperStart = start_[b] + kBlock;
        blocks_.insert(blocks_.begin() + b + 1, std::move(upper));
        start_.insert(start_.begin() + b + 1, upperStart);
    }

    std::vector<std::vector<void*>> blocks_;
    std::vector<int> start_;
    int size_ = 0;
    int lastBlock_ = 0;
};

BlockList g_cache;  // the active s_vertexCache content

inline CompareFn Compare() { return Ref<CompareFn>(kCacheCompare); }

// zCArraySort<zCVertex*>::InsertSort / inlined copies in AddVertex and S_InitVertexMergeCache:
//   lo=0, hi=n-1, mid=(hi)/2; while (lo < hi) { c=cmp(&v,&a[mid]); c>0 ? lo=mid+1 : hi=mid-1; mid=(hi+lo)/2 }
//   if (mid < n) { if (cmp(&v,&a[mid]) > 0) mid++; insert at mid } else a[mid]=v (mid == n)
// Division truncates toward zero like the engine's cdq/sub/sar.
void InsertExact(BlockList& list, void* value) {
    const CompareFn cmp = Compare();
    void* key = value;
    const int n = list.Size();
    int lo = 0;
    int hi = n - 1;
    int sum = hi;
    int mid;
    for (;;) {
        mid = sum / 2;
        if (hi <= lo) break;
        if (cmp(&key, list.At(mid)) > 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
        sum = hi + lo;
    }
    if (mid < n) {
        if (cmp(&key, list.At(mid)) > 0) ++mid;
    }
    list.InsertAt(mid, value);
}

// zCMesh::VertexInMesh, cache branch (0x5683E0..0x5684C7): note the compare happens before the
// loop test, and an equal element ends the search early.
void* FindExact(BlockList& list, const float* position) {
    float* key = &Ref<float>(kFindKey);
    key[0] = position[0];
    key[1] = position[1];
    key[2] = position[2];
    const int n = list.Size();
    if (n <= 0) return nullptr;
    const CompareFn cmp = Compare();
    void* slot = key;
    int lo = 0;
    int hi = n - 1;
    int sum = hi;
    int mid;
    int c;
    for (;;) {
        mid = sum / 2;
        c = cmp(&slot, list.At(mid));
        if (hi <= lo) break;
        if (c > 0) {
            lo = mid + 1;
            sum = hi + lo;
            continue;
        }
        if (c == 0) break;
        hi = mid - 1;
        sum = hi + lo;
    }
    if (c != 0 || mid == -1) return nullptr;
    return *list.At(mid);
}

// ---- reuse of the BSP-mesh cache between MergeMesh calls ---------------------------------------
struct SnapVert {
    void* vertex;
    uint32_t x, y, z;
};
struct SavedCache {
    void* mesh = nullptr;
    BlockList list;
    std::vector<SnapVert> snap;
    void Reset() {
        mesh = nullptr;
        list.Clear();
        std::vector<SnapVert>().swap(snap);
    }
};
SavedCache g_saved;
void* g_mergeMesh = nullptr;   // destination mesh of the running MergeMesh (set at the rebuild site)

struct Stats {
    long long merges = 0, reused = 0, rebuilt = 0, selfChecks = 0, selfCheckFails = 0;
    double rebuildMs = 0, mergeStart = 0;
} g_stats;

bool PrefixMatches(void* mesh) {
    if (g_saved.mesh != mesh) return false;
    const int n = MeshNumVerts(mesh);
    if (n < int(g_saved.snap.size())) return false;
    void** verts = MeshVerts(mesh);
    for (size_t i = 0; i < g_saved.snap.size(); ++i) {
        const SnapVert& s = g_saved.snap[i];
        const uint32_t* p = reinterpret_cast<const uint32_t*>(verts[i]);
        if (verts[i] != s.vertex || p[0] != s.x || p[1] != s.y || p[2] != s.z) return false;
    }
    return true;
}

// ---- source-vertex index for the MergeMesh remap ------------------------------------------------
struct PtrIndexMap {
    std::vector<uint32_t> keys;   // pointer values, 0 = empty
    std::vector<int> values;
    uint32_t mask = 0;
    void* mesh = nullptr;
    void** list = nullptr;
    int count = -1;

    static uint32_t Hash(uint32_t p) {
        p ^= p >> 16;
        p *= 0x7FEB352Du;
        p ^= p >> 15;
        p *= 0x846CA68Bu;
        p ^= p >> 16;
        return p;
    }
    void Build(void* m) {
        mesh = m;
        count = MeshNumVerts(m);
        list = MeshVerts(m);
        uint32_t cap = 16;
        while (cap < uint32_t(count) * 2u + 2u) cap <<= 1;
        keys.assign(cap, 0);
        values.assign(cap, -1);
        mask = cap - 1;
        for (int i = 0; i < count; ++i) {
            const uint32_t key = uint32_t(reinterpret_cast<uintptr_t>(list[i]));
            if (!key) continue;  // a null entry can never equal a polygon vertex we search (handled below)
            uint32_t h = Hash(key) & mask;
            while (keys[h] && keys[h] != key) h = (h + 1) & mask;
            if (!keys[h]) {  // keep the FIRST index, like the engine's forward scan
                keys[h] = key;
                values[h] = i;
            }
        }
    }
    int Find(void* vertex) const {
        const uint32_t key = uint32_t(reinterpret_cast<uintptr_t>(vertex));
        if (!key) return -2;  // let the caller scan
        uint32_t h = Hash(key) & mask;
        while (keys[h]) {
            if (keys[h] == key) return values[h];
            h = (h + 1) & mask;
        }
        return -1;
    }
};
PtrIndexMap g_srcMap;

// ---- hooks (called from the stubs) -------------------------------------------------------------
void __cdecl HookInitCache(void* mesh) {
    // S_InitVertexMergeCache: delete array, compare = 0x572AC0, insert all vertices of mesh in order
    ClearEngineArray();
    Ref<uint32_t>(kCacheCompare) = kCompareVerts;
    g_cache.Clear();
    const int n = MeshNumVerts(mesh);
    void** verts = MeshVerts(mesh);
    for (int i = 0; i < n; ++i) InsertExact(g_cache, verts[i]);
    // the epilogue at kInitBack sets s_usesVertexCache = 1
}

void __cdecl HookDeleteCache() {
    ClearEngineArray();
    g_cache.Clear();
    Ref<int>(kUsesVertexCache) = 0;
}

void* __cdecl HookFind(const float* position) { return FindExact(g_cache, position); }

void __cdecl HookInsert(void* vertex) { InsertExact(g_cache, vertex); }

// replaces "call zCArraySort<zCVertex*>::InsertSort" (thiscall, 1 stack arg) at fixed call sites
void __fastcall HookInsertSort(void* /*array (ecx)*/, void* /*edx*/, void** element) { InsertExact(g_cache, *element); }

void __cdecl HookMergeRebuild(void* mesh, void* /*source*/) {
    // original: DeleteList(&s_vertexCache); compare = 0x572AC0; for i: InsertSort(&verts[i])
    const double t0 = NowMs();
    ClearEngineArray();
    Ref<uint32_t>(kCacheCompare) = kCompareVerts;
    int from = 0;
    const bool reuse = g_reuseMerge && g_saved.mesh == mesh && PrefixMatches(mesh);
    if (reuse) {
        g_cache.Swap(g_saved.list);
        from = int(g_saved.snap.size());
        ++g_stats.reused;
    } else {
        g_cache.Clear();
        ++g_stats.rebuilt;
    }
    g_saved.Reset();
    const int n = MeshNumVerts(mesh);
    void** verts = MeshVerts(mesh);
    for (int i = from; i < n; ++i) InsertExact(g_cache, verts[i]);
    if (reuse && g_selfCheck) {
        BlockList full;
        for (int i = 0; i < n; ++i) InsertExact(full, verts[i]);
        std::vector<void*> a, b;
        full.Flatten(a);
        g_cache.Flatten(b);
        ++g_stats.selfChecks;
        if (a != b) {
            ++g_stats.selfCheckFails;
            Log("LA_FastImport: SELF-CHECK FAILED (reused cache differs from a full rebuild), using the rebuild.");
            g_cache.Swap(full);
        }
    }
    g_mergeMesh = mesh;
    ++g_stats.merges;
    const double t1 = NowMs();
    g_stats.rebuildMs += t1 - t0;
    g_stats.mergeStart = t1;
    if (g_diag) {
        char line[200];
        sprintf_s(line, "MergeMesh #%lld: bsp verts %d, cache %s (%d new inserts), %.1f ms", g_stats.merges, n,
                  reuse ? "reused" : "rebuilt", n - from, t1 - t0);
        Log(line);
    }
}

void __fastcall HookMergeDeleteList(void* /*array (ecx)*/, void* /*edx*/) {
    // original: DeleteList(&s_vertexCache) at the end of MergeMesh (s_bAddVobsToMesh == 0)
    ClearEngineArray();
    void* mesh = g_mergeMesh;
    g_mergeMesh = nullptr;
    if (g_reuseMerge && mesh && g_cache.Size() == MeshNumVerts(mesh)) {
        const int n = MeshNumVerts(mesh);
        void** verts = MeshVerts(mesh);
        g_saved.Reset();
        g_saved.snap.resize(n);
        for (int i = 0; i < n; ++i) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(verts[i]);
            g_saved.snap[i] = SnapVert{verts[i], p[0], p[1], p[2]};
        }
        g_saved.list.Swap(g_cache);
        g_saved.mesh = mesh;
    } else {
        g_saved.Reset();
    }
    g_cache.Clear();
    if (g_diag) {
        char line[160];
        sprintf_s(line, "MergeMesh #%lld done in %.1f ms (cache kept: %s)", g_stats.merges, NowMs() - g_stats.mergeStart,
                  g_saved.mesh ? "yes" : "no");
        Log(line);
    }
}

void __cdecl HookBuildSrcMap(void* source) { g_srcMap.Build(source); }

int __cdecl HookSrcIndex(void* source, void* vertex) {
    const int count = MeshNumVerts(source);
    void** list = MeshVerts(source);
    if (g_srcMap.mesh == source && g_srcMap.list == list && g_srcMap.count == count) {
        const int i = g_srcMap.Find(vertex);
        if (i == -1) return -1;
        if (i >= 0 && i < count && list[i] == vertex) return i;
    }
    for (int i = 0; i < count; ++i) {  // fallback = the engine's scan
        if (list[i] == vertex) return i;
    }
    return -1;
}

// ---- stubs -------------------------------------------------------------------------------------
uint32_t g_initBack = kInitBack;
uint32_t g_findBack = kFindBack;
uint32_t g_addPtrBack = kAddPtrBack;
uint32_t g_addVecBack = kAddVecBack;
uint32_t g_rebuildBack = kMergeRebuildBack;
uint32_t g_mapBack = kMergeMapBack;
uint32_t g_remapBack = kMergeRemapBack;

// S_InitVertexMergeCache at 0x568173: [esp+0x38] = mesh
__declspec(naked) void StubInit() {
    __asm {
        mov eax, [esp + 0x38]
        push eax
        call HookInitCache
        add esp, 4
        jmp dword ptr [g_initBack]
    }
}

// VertexInMesh at 0x5683E0: [esp+0x14] = const zVEC3& (the argument)
__declspec(naked) void StubFind() {
    __asm {
        mov edx, [esp + 0x14]
        push edx
        call HookFind
        add esp, 4
        jmp dword ptr [g_findBack]
    }
}

// AddVertex(zCVertex*) at 0x56880E: [esp+0x18] = the vertex pointer argument
__declspec(naked) void StubAddPtr() {
    __asm {
        mov eax, [esp + 0x18]
        push eax
        call HookInsert
        add esp, 4
        jmp dword ptr [g_addPtrBack]
    }
}

// AddVertex(zVEC3&) at 0x568A19: ebx = mesh, new vertex = verts[numVert]
__declspec(naked) void StubAddVec() {
    __asm {
        mov edx, [ebx + 0x38]
        mov eax, [ebx + 0x40]
        mov eax, [eax + edx * 4]
        push eax
        call HookInsert
        add esp, 4
        jmp dword ptr [g_addVecBack]
    }
}

// MergeMesh at 0x56AB31: ebp = this (BSP mesh), esi = source mesh
__declspec(naked) void StubMergeRebuild() {
    __asm {
        push esi
        push ebp
        call HookMergeRebuild
        add esp, 8
        jmp dword ptr [g_rebuildBack]
    }
}

// MergeMesh at 0x56AB77: esi = source mesh; re-executes "mov eax,[esi+38h]; shl eax,2"
__declspec(naked) void StubMergeMap() {
    __asm {
        push esi
        call HookBuildSrcMap
        add esp, 4
        mov eax, [esi + 0x38]
        shl eax, 2
        jmp dword ptr [g_mapBack]
    }
}

// MergeMesh at 0x56ADF1: esi = source mesh, edi = polygon vertex, ecx = vertex slot, ebx = new polygon,
// [esp+14h] = remap array. Found: poly->vertex[ecx] = remap[index].
__declspec(naked) void StubMergeRemap() {
    __asm {
        push ecx
        push edi
        push esi
        call HookSrcIndex
        add esp, 8
        pop ecx
        test eax, eax
        js not_found
        mov edx, [esp + 0x14]
        mov eax, [edx + eax * 4]
        mov edx, [ebx]
        mov [edx + ecx * 4], eax
    not_found:
        jmp dword ptr [g_remapBack]
    }
}

// ---- code check --------------------------------------------------------------------------------
// Every engine byte range the plugin relies on (patched functions, comparator, InsertSort,
// DeleteList, operator delete[] thunk, the LoadMSH call site).
struct CodeRange {
    uint32_t begin;
    uint32_t end;
};
constexpr CodeRange kCodeRanges[] = {
    {0x005680A0, 0x0056831A},  // zCMesh::S_InitVertexMergeCache
    {0x00568320, 0x00568345},  // zCMesh::S_DeleteVertexMergeCache
    {0x00568350, 0x005684D8},  // zCMesh::VertexInMesh
    {0x005686B0, 0x005688EA},  // zCMesh::AddVertex(zCVertex*)
    {0x005688F0, 0x00568B71},  // zCMesh::AddVertex(zVEC3&)
    {0x0056A9B0, 0x0056AE8B},  // zCMesh::MergeMesh
    {0x00570CB6, 0x00570CCD},  // zCMesh::LoadMSH, InsertSort call
    {0x00572AC0, 0x00572CE3},  // vertex compare
    {0x00575830, 0x00575975},  // zCArraySort<zCVertex*>::InsertSort
    {0x00575980, 0x0057599A},  // zCArraySort<zCVertex*>::DeleteList
    {0x00565F80, 0x00565F85},  // operator delete[]
    {0x00538900, 0x00538A60},  // zCBspTree::SaveBIN part (0x538010): chunk length check
    {0x00538EE0, 0x00539000},  // zCBspTree::LoadBIN part: chunk length check
};
#ifndef LAFI_EXPECTED_FINGERPRINT
#define LAFI_EXPECTED_FINGERPRINT 0x2D47E1ABu  // Steam Gothic2.exe 2.6 fix, 9,038,140 bytes
#endif
constexpr uint32_t kExpectedFingerprint = LAFI_EXPECTED_FINGERPRINT;

uint32_t CodeFingerprint(const uint8_t* image) {
    uint32_t hash = 2166136261u;
    for (const CodeRange& range : kCodeRanges) {
        for (uint32_t va = range.begin; va < range.end; ++va) hash = (hash ^ image[va - kImageBase]) * 16777619u;
    }
    return hash;
}

uint32_t ExeFileSize() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const DWORD size = GetFileSize(file, nullptr);
    CloseHandle(file);
    return size == INVALID_FILE_SIZE ? 0 : size;
}

uint32_t g_lastFingerprint = 0;

bool SameCode(bool checkModule) {
    char line[300];
    if (checkModule && reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        Log("LA_FastImport " LAFI_VERSION " loaded. Exe is not at 0x400000, so it is not Gothic2.exe. No patch.");
        return false;
    }
    uint32_t fingerprint = 0;
    __try {
        fingerprint = CodeFingerprint(reinterpret_cast<const uint8_t*>(kImageBase));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fingerprint = 0;
    }
    g_lastFingerprint = fingerprint;
    const bool same = fingerprint == kExpectedFingerprint;
    sprintf_s(line, "LA_FastImport " LAFI_VERSION " loaded. Gothic2.exe %lu bytes, code fingerprint 0x%08lX (expected 0x%08lX)%s",
              static_cast<unsigned long>(checkModule ? ExeFileSize() : 0), static_cast<unsigned long>(fingerprint),
              static_cast<unsigned long>(kExpectedFingerprint),
              same ? "." : ". Code does not match 2.6 fix or was changed by another patch. No patch.");
    Log(line);
    return same;
}

// ---- ini ---------------------------------------------------------------------------------------
bool IniPath(wchar_t* path, bool besideDll) {
    HMODULE module = besideDll ? g_module : nullptr;
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"LA_FastImport.ini");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

int ReadInt(const wchar_t* ini, const wchar_t* key, int def, int lo, int hi) {
    const int v = int(GetPrivateProfileIntW(L"LA_FASTIMPORT", key, def, ini));
    return (v < lo || v > hi) ? def : v;
}

void LoadConfig() {
    wchar_t ini[MAX_PATH];
    if (!(g_module && IniPath(ini, true)) && !IniPath(ini, false)) return;  // defaults
    g_enabled = ReadInt(ini, L"Enabled", 1, 0, 1);
    g_fastCache = ReadInt(ini, L"FastVertexCache", 1, 0, 1);
    g_reuseMerge = ReadInt(ini, L"ReuseMergeCache", 1, 0, 1);
    g_fastRemap = ReadInt(ini, L"FastMergeRemap", 1, 0, 1);
    g_allowBigBsp = ReadInt(ini, L"AllowBigBsp", 1, 0, 1);
    g_selfCheck = ReadInt(ini, L"SelfCheck", 0, 0, 1);
    g_diag = ReadInt(ini, L"Diag", 0, 0, 1);
    g_diagMaxLines = ReadInt(ini, L"DiagMaxLines", 2000, 1, 1000000);
}

// ---- patching ----------------------------------------------------------------------------------
struct Site {
    uint32_t address;
    uint8_t original[10];
    uint8_t size;
    uint8_t saved[10];
    bool active;
};

bool Write(uint32_t address, const void* bytes, size_t size) {
    void* p = reinterpret_cast<void*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return true;
}

// jmp/call rel32 from site to target, padded with nops to size
void MakeBranch(uint8_t opcode, uint32_t site, const void* target, uint8_t* out, size_t size) {
    memset(out, 0x90, size);
    out[0] = opcode;
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(target)) - int32_t(site + 5);
    memcpy(out + 1, &rel, 4);
}

struct PatchDef {
    uint32_t address;
    uint8_t size;
    uint8_t original[10];
    uint8_t opcode;      // 0xE9 jmp, 0xE8 call
    const void* target;
    int group;           // 0 cache, 2 remap, 3 big BSP
};

std::vector<PatchDef> Patches() {
    return {
        {kInitSite, 6, {0x8B, 0x15, 0xB8, 0x87, 0x8D, 0x00}, 0xE9, reinterpret_cast<const void*>(&StubInit), 0},
        {kDeleteSite, 5, {0xA1, 0xB8, 0x87, 0x8D, 0x00}, 0xE9, reinterpret_cast<const void*>(&HookDeleteCache), 0},
        {kFindSite, 6, {0x8B, 0x54, 0x24, 0x14, 0x8B, 0x02}, 0xE9, reinterpret_cast<const void*>(&StubFind), 0},
        {kAddPtrSite, 6, {0x8B, 0x0D, 0xC0, 0x87, 0x8D, 0x00}, 0xE9, reinterpret_cast<const void*>(&StubAddPtr), 0},
        {kAddVecSite, 6, {0x8B, 0x53, 0x38, 0x8B, 0x43, 0x40}, 0xE9, reinterpret_cast<const void*>(&StubAddVec), 0},
        {kMergeRebuildSite, 10, {0xB9, 0xB8, 0x87, 0x8D, 0x00, 0xE8, 0x45, 0xAE, 0x00, 0x00}, 0xE9,
         reinterpret_cast<const void*>(&StubMergeRebuild), 0},
        {kMergeInsertCall, 5, {0xE8, 0x4D, 0xAB, 0x00, 0x00}, 0xE8, reinterpret_cast<const void*>(&HookInsertSort), 0},
        {kLoadMshInsertCall, 5, {0xE8, 0x63, 0x4B, 0x00, 0x00}, 0xE8, reinterpret_cast<const void*>(&HookInsertSort), 0},
        {kMergeDeleteCall, 5, {0xE8, 0x1A, 0xAB, 0x00, 0x00}, 0xE8, reinterpret_cast<const void*>(&HookMergeDeleteList), 0},
        {kMergeMapSite, 6, {0x8B, 0x46, 0x38, 0xC1, 0xE0, 0x02}, 0xE9, reinterpret_cast<const void*>(&StubMergeMap), 2},
        {kMergeRemapSite, 5, {0x8B, 0x56, 0x38, 0x33, 0xC0}, 0xE9, reinterpret_cast<const void*>(&StubMergeRemap), 2},
        // "if (chunkLen <= 90,000,000) skip the fault report" -> always skip (jbe -> jmp, same target)
        {kSaveBinCheck, 6, {0x0F, 0x86, 0x23, 0x01, 0x00, 0x00}, 0xE9, reinterpret_cast<const void*>(kSaveBinOk), 3},
        {kLoadBinCheck, 6, {0x0F, 0x86, 0xE1, 0x00, 0x00, 0x00}, 0xE9, reinterpret_cast<const void*>(kLoadBinOk), 3},
    };
}

struct Applied {
    uint32_t address;
    uint8_t size;
    uint8_t saved[10];
};
std::vector<Applied> g_applied;

void Remove() {
    for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) Write(it->address, it->saved, it->size);
    g_applied.clear();
    g_patchedCache = g_patchedMerge = g_patchedRemap = 0;
    g_cache.Clear();
    g_saved.Reset();
    g_mergeMesh = nullptr;
}

// checkModule=false only in the test harness (exe image mapped by hand at 0x400000)
bool Install(bool checkModule = true) {
    if (checkModule) LoadConfig();
    if (g_diag) OpenLog();  // now, while the process is still starting
    if (!SameCode(checkModule)) return false;
    if (!g_enabled) {
        Log("LA_FastImport: Enabled=0, no patch.");
        return false;
    }
    // the reuse needs the fast cache (it keeps BlockList objects)
    if (!g_fastCache) g_reuseMerge = 0;
    const std::vector<PatchDef> defs = Patches();
    for (const PatchDef& d : defs) {
        if (memcmp(reinterpret_cast<const void*>(d.address), d.original, d.size) != 0) {
            char line[120];
            sprintf_s(line, "LA_FastImport: bytes at 0x%08lX differ. No patch.", static_cast<unsigned long>(d.address));
            Log(line);
            return false;
        }
    }
    // the four call sites must load ecx = &s_vertexCache right before the call
    const uint8_t kMovEcxCache[5] = {0xB9, 0xB8, 0x87, 0x8D, 0x00};
    if (memcmp(reinterpret_cast<const void*>(kMergeInsertCall - 5), kMovEcxCache, 5) != 0 ||
        memcmp(reinterpret_cast<const void*>(kLoadMshInsertCall - 5), kMovEcxCache, 5) != 0 ||
        memcmp(reinterpret_cast<const void*>(kMergeDeleteCall - 5), kMovEcxCache, 5) != 0) {
        Log("LA_FastImport: call sites differ. No patch.");
        return false;
    }
    if (g_cache.Size() || Ref<int>(kCacheNum) != 0 || Ref<int>(kUsesVertexCache) != 0) {
        Log("LA_FastImport: vertex cache is in use right now. No patch.");
        return false;
    }
    for (const PatchDef& d : defs) {
        if (d.group == 0 && !g_fastCache) continue;
        if (d.group == 2 && !g_fastRemap) continue;
        if (d.group == 3 && !g_allowBigBsp) continue;
        Applied a{d.address, d.size, {}};
        memcpy(a.saved, reinterpret_cast<const void*>(d.address), d.size);
        uint8_t bytes[10];
        MakeBranch(d.opcode, d.address, d.target, bytes, d.size);
        if (!Write(d.address, bytes, d.size)) {
            Remove();
            Log("LA_FastImport: writing failed. No patch.");
            return false;
        }
        g_applied.push_back(a);
    }
    g_patchedCache = g_fastCache;
    g_patchedMerge = g_reuseMerge;
    g_patchedRemap = g_fastRemap;
    char line[200];
    sprintf_s(line, "LA_FastImport " LAFI_VERSION ": active. FastVertexCache=%d ReuseMergeCache=%d FastMergeRemap=%d AllowBigBsp=%d SelfCheck=%d",
              g_fastCache, g_reuseMerge, g_fastRemap, g_allowBigBsp, g_selfCheck);
    Log(line);
    return true;
}

void LogSummary() {
    if (!g_diag || !g_stats.merges) return;
    char line[200];
    sprintf_s(line, "LA_FastImport summary: %lld MergeMesh calls, cache reused %lld, rebuilt %lld, %.0f ms in cache rebuilds",
              g_stats.merges, g_stats.reused, g_stats.rebuilt, g_stats.rebuildMs);
    Log(line);
}

}  // namespace lafi

#ifndef LAFI_NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        lafi::g_module = module;
        lafi::Install();
    } else if (reason == DLL_PROCESS_DETACH) {
        lafi::LogSummary();  // also at process exit (reserved != NULL): the compile run ends with ExitGame
        if (reserved == nullptr) lafi::Remove();
        lafi::CloseLog();
    }
    return TRUE;
}
#endif
