// LA_FastImport test harness (own code, MIT like the plugin).
// Maps Gothic2.exe (read-only file access) at 0x400000 into this 32-bit process, resolves its imports
// (system DLLs from System32; SmartHeap -> own heap; game DLLs such as ddraw/mss32/binkw32/vdfs32g ->
// stubs that are never loaded), runs the exe's CRT/static initializers and then calls the ENGINE'S OWN
// machine code (zCMesh::S_InitVertexMergeCache, AddVertexSmart, AddPoly, MergeMesh, ...) on our real
// level meshes. Every test runs once with the original code and once with the LA_FastImport patches,
// and compares the resulting vertex/polygon lists bit by bit. Gothic is never started, no window,
// no GPU.
//
// Usage: harness.exe <Gothic2.exe> <test> <mode> [options]
//   test  import <list.txt>           : each 3DS = one zCMesh built like zCFile3DS (cache + AddVertexSmart + AddPoly)
//         vobload <list.txt>          : game-time vob visual load: real zCMesh objects, import, destroy,
//                                       zCVertex pool free-list check after every file (v1.2)
//         merge  <zen> <meshdir>      : tiles in the order of the ZEN's level vobs, merged into one BSP mesh
//                                       with zCMesh::MergeMesh (what zCCBspTree::AddMesh does)
//   mode  orig | patched | both
//   -max N      : at most N files/tiles (orig run), -maxp N for the patched run (default: all)
//   -tlimit S   : stop the orig run after S seconds (then compare the common prefix)
//   -selfcheck  : plugin SelfCheck=1
//   -noreuse / -noremap / -nocache : switch plugin parts off;  -diag : plugin log next to the harness exe
//   harness.exe <Gothic2.exe> patchcheck  : every patch site original -> patched -> restored; BIN checks jbe -> jmp same target
//   harness.exe <Gothic2.exe> fingerprint : code fingerprint of kCodeRanges
#define LAFI_NO_DLLMAIN
#include "../src/la_fastimport.cpp"
#include "../src/la_exe_image.h"

#include <map>
#include <string>

namespace {

// Loader (exe mapping, imports, CRT/static init, 0x400000 reservation): src/la_exe_image.h
using laimg::H_calloc;
using laimg::ReadFileAll;

// ---------------------------------------------------------------------------------------- engine
using FnInitCache = void(__cdecl*)(void* mesh);
using FnDeleteCache = void(__cdecl*)();
using FnAddVertexSmart = void*(__thiscall*)(void* mesh, const float* v);
using FnAddPoly3 = void*(__thiscall*)(void* mesh, void* a, void* b, void* c);
using FnAllocVerts = void(__thiscall*)(void* mesh, int n);
using FnAllocPolys = void(__thiscall*)(void* mesh, int n);
using FnMergeMesh = void(__thiscall*)(void* mesh, void* src, const float* matrix);
const auto E_InitCache = reinterpret_cast<FnInitCache>(0x005680A0);
const auto E_DeleteCache = reinterpret_cast<FnDeleteCache>(0x00568320);
const auto E_AddVertexSmart = reinterpret_cast<FnAddVertexSmart>(0x00568B80);
const auto E_AddPoly3 = reinterpret_cast<FnAddPoly3>(0x00568EA0);
const auto E_AllocVerts = reinterpret_cast<FnAllocVerts>(0x00568500);
const auto E_AllocPolys = reinterpret_cast<FnAllocPolys>(0x005685F0);
const auto E_MergeMesh = reinterpret_cast<FnMergeMesh>(0x0056A9B0);

void* NewMesh() { return H_calloc(1, 0xE4); }
int NumVerts(void* m) { return lafi::Member<int>(m, 0x38); }
int NumPolys(void* m) { return lafi::Member<int>(m, 0x34); }
void** Verts(void* m) { return lafi::Member<void**>(m, 0x40); }
void** Polys(void* m) { return lafi::Member<void**>(m, 0x44); }

// --------------------------------------------------------------------------------------- 3DS read
struct Mesh3ds {
    std::vector<float> pos;          // x,y,z (Gothic axes)
    std::vector<uint32_t> faces;     // a,b,c (global vertex indices)
};

void Parse3dsChunks(const uint8_t* p, const uint8_t* end, Mesh3ds& m, uint32_t& base) {
    while (p + 6 <= end) {
        const uint16_t id = *reinterpret_cast<const uint16_t*>(p);
        const uint32_t len = *reinterpret_cast<const uint32_t*>(p + 2);
        if (len < 6 || p + len > end) return;
        const uint8_t* data = p + 6;
        const uint8_t* next = p + len;
        switch (id) {
            case 0x4D4D: case 0x3D3D: Parse3dsChunks(data, next, m, base); break;
            case 0x4000: {  // object: name, then sub chunks
                const uint8_t* q = data;
                while (q < next && *q) ++q;
                Parse3dsChunks(q + 1, next, m, base);
                break;
            }
            case 0x4100:  // tri mesh: vertex chunk first, faces index this object's vertices (from base)
                Parse3dsChunks(data, next, m, base);
                break;
            case 0x4110: {
                const uint16_t n = *reinterpret_cast<const uint16_t*>(data);
                const float* v = reinterpret_cast<const float*>(data + 2);
                for (int i = 0; i < n; ++i) {  // 3ds z-up -> Gothic y-up
                    m.pos.push_back(v[3 * i + 0]);
                    m.pos.push_back(v[3 * i + 2]);
                    m.pos.push_back(v[3 * i + 1]);
                }
                break;
            }
            case 0x4120: {
                const uint16_t n = *reinterpret_cast<const uint16_t*>(data);
                const uint16_t* f = reinterpret_cast<const uint16_t*>(data + 2);
                for (int i = 0; i < n; ++i) {
                    m.faces.push_back(base + f[4 * i + 0]);
                    m.faces.push_back(base + f[4 * i + 1]);
                    m.faces.push_back(base + f[4 * i + 2]);
                }
                break;
            }
            default: break;
        }
        if (id == 0x4100) base = uint32_t(m.pos.size() / 3);
        p = next;
    }
}

bool Load3ds(const std::string& path, Mesh3ds& m) {
    std::vector<uint8_t> data;
    if (!ReadFileAll(path.c_str(), data)) return false;
    uint32_t base = 0;
    Parse3dsChunks(data.data(), data.data() + data.size(), m, base);
    return !m.pos.empty();
}

// ----------------------------------------------------------------------------------- operations
double Now() { return lafi::NowMs(); }

struct ImportResult {
    std::vector<uint32_t> vertexBits;   // positions of the mesh vertex list
    std::vector<int> mapping;           // 3ds vertex -> mesh vertex index
    std::vector<int> polyIdx;           // per polygon 3 vertex indices
    double ms = 0;
};

// what zCFile3DS::Load3DS + FlushMeshBuffer (0x55D090) do with the vertex/face buffer
void* BuildMesh(const Mesh3ds& m, ImportResult* out) {
    void* mesh = NewMesh();
    const int nv = int(m.pos.size() / 3);
    const int nf = int(m.faces.size() / 3);
    const double t0 = Now();
    E_InitCache(mesh);
    E_AllocVerts(mesh, nv);
    std::vector<void*> vptr(nv);
    for (int i = 0; i < nv; ++i) vptr[i] = E_AddVertexSmart(mesh, &m.pos[3 * i]);
    E_AllocPolys(mesh, nf);
    for (int f = 0; f < nf; ++f) E_AddPoly3(mesh, vptr[m.faces[3 * f]], vptr[m.faces[3 * f + 1]], vptr[m.faces[3 * f + 2]]);
    E_DeleteCache();
    const double t1 = Now();
    if (out) {
        out->ms = t1 - t0;
        std::map<void*, int> index;
        const int n = NumVerts(mesh);
        void** verts = Verts(mesh);
        out->vertexBits.resize(3 * n);
        for (int i = 0; i < n; ++i) {
            index[verts[i]] = i;
            memcpy(&out->vertexBits[3 * i], verts[i], 12);
        }
        out->mapping.resize(nv);
        for (int i = 0; i < nv; ++i) out->mapping[i] = index.count(vptr[i]) ? index[vptr[i]] : -1;
        void** polys = Polys(mesh);
        out->polyIdx.clear();
        for (int p = 0; p < NumPolys(mesh); ++p) {
            void** pv = *reinterpret_cast<void***>(polys[p]);
            const int cnt = reinterpret_cast<uint8_t*>(polys[p])[0x30];
            for (int k = 0; k < cnt; ++k) out->polyIdx.push_back(index.count(pv[k]) ? index[pv[k]] : -1);
        }
    }
    return mesh;
}

struct MergeResult {
    std::vector<uint32_t> vertexBits;
    std::vector<int> polyIdx;
    std::vector<uint32_t> polyPlane;    // normal + d bits per polygon
    std::vector<double> tileMs;
    int tiles = 0;
};

void Snapshot(void* bsp, MergeResult& r) {
    const int n = NumVerts(bsp);
    void** verts = Verts(bsp);
    std::vector<std::pair<void*, int>> order(n);
    for (int i = 0; i < n; ++i) order[i] = {verts[i], i};
    std::sort(order.begin(), order.end());
    auto indexOf = [&](void* v) {
        auto it = std::lower_bound(order.begin(), order.end(), std::make_pair(v, INT_MIN));
        return (it != order.end() && it->first == v) ? it->second : -1;
    };
    r.vertexBits.resize(3 * n);
    for (int i = 0; i < n; ++i) memcpy(&r.vertexBits[3 * i], verts[i], 12);
    void** polys = Polys(bsp);
    r.polyIdx.clear();
    r.polyPlane.clear();
    for (int p = 0; p < NumPolys(bsp); ++p) {
        const uint8_t* poly = reinterpret_cast<const uint8_t*>(polys[p]);
        void** pv = *reinterpret_cast<void** const*>(poly);
        const int cnt = poly[0x30];
        r.polyIdx.push_back(cnt);
        for (int k = 0; k < cnt; ++k) r.polyIdx.push_back(indexOf(pv[k]));
        const uint32_t* plane = reinterpret_cast<const uint32_t*>(poly + 8);
        for (int k = 0; k < 4; ++k) r.polyPlane.push_back(plane[k]);
    }
}

uint64_t Fnv(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
template <class T> uint64_t FnvVec(const std::vector<T>& v, uint64_t h = 1469598103934665603ull) {
    return v.empty() ? h : Fnv(v.data(), v.size() * sizeof(T), h);
}

// ------------------------------------------------------------------------- game-time vob load (v1.2)
// What happens per vob visual at world load with -zautoconvertdata: a REAL zCMesh object
// (zCMesh::_CreateNewInstance), the 3DS import (cache init, AddVertexSmart, AddPoly, cache delete),
// CalcBBox3D, then the mesh is destroyed (zCMesh::`vector deleting destructor` -> DeleteAll ->
// every zCVertex back into zCMemPool<zCVertex> at 0x008D4304) and the next file reuses the freed slots.
// After every file the vertex pool free list is walked (next/prev links, count); a crash seen in the game was a
// corrupted free list of exactly this pool.
using FnCreateMesh = void*(__cdecl*)();
using FnCalcBBox = void(__thiscall*)(void* mesh, int fast);
const auto E_CreateMesh = reinterpret_cast<FnCreateMesh>(0x00566CD0);
const auto E_MeshDeleteAll = reinterpret_cast<void(__thiscall*)(void*)>(0x00567290);  // zCMesh::DeleteAll (called by ~zCMesh)
const auto E_CalcBBox3D = reinterpret_cast<FnCalcBBox>(0x0056A610);
constexpr uint32_t kVertexPool = 0x008D4304;  // zCMemPool<zCVertex>: +0 item size, +0xC free head, +0x10 free count

// walks the free list; 0 ok, else error code with *badNode / *count filled (no C++ objects: __try)
int WalkVertexPool(int* count, int* expected, void** badNode) {
    uint8_t* pool = reinterpret_cast<uint8_t*>(kVertexPool);
    *expected = *reinterpret_cast<int*>(pool + 0x10);
    void** link = reinterpret_cast<void**>(pool + 0xC);  // address of the "next" field of the predecessor
    int n = 0;
    *badNode = nullptr;
    __try {
        for (void** node = static_cast<void**>(*link); node; node = static_cast<void**>(node[0])) {
            if (node[1] != reinterpret_cast<void*>(link)) { *badNode = node; *count = n; return 1; }
            link = node;
            if (++n > *expected + 1) { *count = n; return 2; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *count = n;
        return 3;
    }
    *count = n;
    return n == *expected ? 0 : 4;
}

// returns "" if the free list is consistent, else a description
std::string CheckVertexPool(int* freeCount) {
    int expected = 0;
    void* bad = nullptr;
    const int r = WalkVertexPool(freeCount, &expected, &bad);
    char b[200] = "";
    if (r == 1) sprintf_s(b, "free node %p has a wrong prev link after %d nodes (data %08X %08X)", bad, *freeCount,
                          static_cast<uint32_t*>(bad)[0], static_cast<uint32_t*>(bad)[1]);
    if (r == 2) sprintf_s(b, "free list longer than the free count %d (cycle?)", expected);
    if (r == 3) sprintf_s(b, "access violation while walking the free list after %d nodes", *freeCount);
    if (r == 4) sprintf_s(b, "free list has %d nodes, pool counts %d", *freeCount, expected);
    return b;
}
struct VobLoadResult {
    std::vector<uint64_t> fileHash;   // per file: vertices + mapping + polygon indices
    int poolErrors = 0;
    int files = 0;
};

void VobLoadPass(const std::vector<std::string>& files, const std::vector<Mesh3ds>& meshes, bool withPlugin, VobLoadResult& res) {
    const double start = Now();
    for (size_t i = 0; i < files.size(); ++i) {
        void* mesh = E_CreateMesh();
        const Mesh3ds& m = meshes[i];
        const int nv = int(m.pos.size() / 3);
        const int nf = int(m.faces.size() / 3);
        E_InitCache(mesh);
        E_AllocVerts(mesh, nv);
        std::vector<void*> vptr(nv);
        for (int k = 0; k < nv; ++k) vptr[k] = E_AddVertexSmart(mesh, &m.pos[3 * k]);
        E_AllocPolys(mesh, nf);
        for (int f = 0; f < nf; ++f) E_AddPoly3(mesh, vptr[m.faces[3 * f]], vptr[m.faces[3 * f + 1]], vptr[m.faces[3 * f + 2]]);
        E_DeleteCache();
        E_CalcBBox3D(mesh, 0);
        // fingerprint of the result (index based, independent of addresses)
        std::map<void*, int> index;
        const int n = NumVerts(mesh);
        void** verts = Verts(mesh);
        uint64_t h = 1469598103934665603ull;
        int foreign = 0;
        for (int k = 0; k < n; ++k) {
            index[verts[k]] = k;
            h = Fnv(verts[k], 12, h);
        }
        for (int k = 0; k < nv; ++k) {
            const auto it = index.find(vptr[k]);
            const int idx = it == index.end() ? -1 : it->second;
            if (idx < 0) ++foreign;
            h = Fnv(&idx, 4, h);
        }
        void** polys = Polys(mesh);
        for (int p = 0; p < NumPolys(mesh); ++p) {
            void** pv = *reinterpret_cast<void***>(polys[p]);
            const int cnt = reinterpret_cast<uint8_t*>(polys[p])[0x30];
            for (int k = 0; k < cnt; ++k) {
                const auto it = index.find(pv[k]);
                const int idx = it == index.end() ? -1 : it->second;
                if (idx < 0) ++foreign;
                h = Fnv(&idx, 4, h);
            }
        }
        const int np = NumPolys(mesh);
        E_MeshDeleteAll(mesh);  // frees all vertices/polygons into the pools (the object shell is leaked)
        int freeNodes = 0;
        const std::string pool = CheckVertexPool(&freeNodes);
        res.fileHash.push_back(h);
        res.files = int(i) + 1;
        const bool bad = !pool.empty() || foreign;
        if (bad) ++res.poolErrors;
        if (bad || i % 100 == 0 || i + 1 == files.size())
            printf("  %-8s %4zu %-46s verts %6d polys %6d foreign %d pool free %7d %s  %.1f s\n", withPlugin ? "patched" : "orig", i,
                   files[i].substr(files[i].find_last_of("\\/") + 1).c_str(), n, np, foreign, freeNodes,
                   pool.empty() ? "ok" : pool.c_str(), (Now() - start) / 1000.0);
        fflush(stdout);
    }
}

int RunVobLoad(const std::vector<std::string>& files, const std::string& mode) {
    std::vector<Mesh3ds> meshes(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        if (!Load3ds(files[i], meshes[i])) { fprintf(stderr, "cannot read 3ds %s\n", files[i].c_str()); return 2; }
    }
    printf("vobload: %zu files (create mesh, import, cache delete, bbox, destroy; pool check after each)\n", files.size());
    VobLoadResult orig, patched;
    if (mode != "patched") VobLoadPass(files, meshes, false, orig);
    if (mode != "orig") {
        if (!lafi::Install(false)) { fprintf(stderr, "plugin install failed (fingerprint 0x%08X)\n", lafi::g_lastFingerprint); return 4; }
        VobLoadPass(files, meshes, true, patched);
        printf("  plugin after the pass: cache size %d, engine array %p num %d, usesCache %d\n", lafi::g_cache.Size(),
               lafi::Ref<void*>(lafi::kCacheArray), lafi::Ref<int>(lafi::kCacheNum), lafi::Ref<int>(lafi::kUsesVertexCache));
        lafi::Remove();
    }
    int diff = 0;
    if (mode == "both") {
        for (size_t i = 0; i < files.size(); ++i)
            if (orig.fileHash[i] != patched.fileHash[i]) {
                ++diff;
                printf("  DIFF in %s\n", files[i].c_str());
            }
    }
    printf("RESULT vobload: %zu files, orig pool/foreign errors %d, patched pool/foreign errors %d, %d differ\n", files.size(),
           orig.poolErrors, patched.poolErrors, diff);
    return (orig.poolErrors || patched.poolErrors || diff) ? 1 : 0;
}

std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return out;
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') out.push_back(s);
    }
    fclose(f);
    return out;
}

// level tiles in the order of "visual=string:<NAME>.3DS" in the ZEN
std::vector<std::string> TilesFromZen(const char* zen, const std::string& dir) {
    std::vector<uint8_t> data;
    std::vector<std::string> out;
    if (!ReadFileAll(zen, data)) return out;
    const std::string text(data.begin(), data.end());
    const std::string tag = "visual=string:";
    size_t pos = 0;
    while ((pos = text.find(tag, pos)) != std::string::npos) {
        pos += tag.size();
        size_t e = pos;
        while (e < text.size() && text[e] != '\r' && text[e] != '\n' && text[e] != ' ' && text[e] != '\t') ++e;
        std::string name = text.substr(pos, e - pos);
        std::string up = name;
        for (auto& c : up) c = char(toupper(c));
        if (up.size() > 4 && up.substr(up.size() - 4) == ".3DS") {
            std::string path = dir + "\\" + name;  // tiles; untiled level meshes lie one folder up
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = dir + "\\..\\" + name;
            out.push_back(path);
        }
    }
    return out;
}

struct Options {
    int maxOrig = INT_MAX, maxPatched = INT_MAX;
    double tlimit = 1e30;
};

int RunImport(const std::vector<std::string>& files, const std::string& mode, const Options& opt) {
    std::vector<ImportResult> orig, patched;
    std::vector<Mesh3ds> meshes(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        if (!Load3ds(files[i], meshes[i])) { fprintf(stderr, "cannot read 3ds %s\n", files[i].c_str()); return 2; }
    }
    auto pass = [&](bool withPlugin, int maxFiles, std::vector<ImportResult>& res) {
        const double start = Now();
        for (size_t i = 0; i < files.size() && int(i) < maxFiles; ++i) {
            if (!withPlugin && (Now() - start) / 1000.0 > opt.tlimit) break;
            ImportResult r;
            BuildMesh(meshes[i], &r);
            printf("  %-8s %-48s 3ds verts %7zu -> mesh verts %7zu polys %7zu  %10.1f ms\n", withPlugin ? "patched" : "orig",
                   files[i].substr(files[i].find_last_of("\\/") + 1).c_str(), meshes[i].pos.size() / 3, r.vertexBits.size() / 3,
                   r.polyIdx.size() / 3, r.ms);
            fflush(stdout);
            res.push_back(std::move(r));
        }
    };
    if (mode != "patched") pass(false, opt.maxOrig, orig);
    if (mode != "orig") {
        if (!lafi::Install(false)) { fprintf(stderr, "plugin install failed (fingerprint 0x%08X)\n", lafi::g_lastFingerprint); return 4; }
        pass(true, opt.maxPatched, patched);
        lafi::Remove();
    }
    if (mode == "both") {
        const size_t n = (std::min)(orig.size(), patched.size());
        int diff = 0;
        double to = 0, tp = 0;
        for (size_t i = 0; i < n; ++i) {
            const bool same = orig[i].vertexBits == patched[i].vertexBits && orig[i].mapping == patched[i].mapping &&
                              orig[i].polyIdx == patched[i].polyIdx;
            if (!same) {
                ++diff;
                printf("  DIFF in %s\n", files[i].c_str());
            }
            to += orig[i].ms;
            tp += patched[i].ms;
        }
        printf("RESULT import: %zu files compared, %d differ; orig %.1f ms, patched %.1f ms, speed-up %.1fx\n", n, diff, to, tp,
               tp > 0 ? to / tp : 0.0);
        return diff ? 1 : 0;
    }
    return 0;
}

int RunMerge(const std::vector<std::string>& tiles, const std::string& mode, const Options& opt) {
    std::vector<Mesh3ds> meshes(tiles.size());
    size_t totalFaces = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (!Load3ds(tiles[i], meshes[i])) { fprintf(stderr, "cannot read 3ds %s\n", tiles[i].c_str()); return 2; }
        totalFaces += meshes[i].faces.size() / 3;
    }
    printf("merge: %zu tiles, %zu faces\n", tiles.size(), totalFaces);
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MergeResult orig, patched;
    auto pass = [&](bool withPlugin, int maxTiles, MergeResult& res) {
        void* bsp = NewMesh();
        const double start = Now();
        double sum = 0;
        for (size_t i = 0; i < tiles.size() && int(i) < maxTiles; ++i) {
            if (!withPlugin && (Now() - start) / 1000.0 > opt.tlimit) break;
            void* tile = BuildMesh(meshes[i], nullptr);
            const double t0 = Now();
            E_MergeMesh(bsp, tile, identity);
            const double ms = Now() - t0;
            sum += ms;
            res.tileMs.push_back(ms);
            res.tiles = int(i) + 1;
            printf("  %-8s %3zu %-40s faces %6zu  bsp verts %8d polys %8d  merge %9.1f ms  sum %9.1f s\n", withPlugin ? "patched" : "orig", i,
                   tiles[i].substr(tiles[i].find_last_of("\\/") + 1).c_str(), meshes[i].faces.size() / 3, NumVerts(bsp), NumPolys(bsp), ms,
                   sum / 1000.0);
            fflush(stdout);
            if (withPlugin && mode == "both" && res.tiles == orig.tiles) Snapshot(bsp, res);  // same tile count as orig
        }
        if (!(withPlugin && mode == "both")) Snapshot(bsp, res);
        printf("  %s total merge time %.1f s for %d tiles\n", withPlugin ? "patched" : "orig", sum / 1000.0, res.tiles);
    };
    if (mode != "patched") pass(false, opt.maxOrig, orig);
    if (mode != "orig") {
        if (!lafi::Install(false)) { fprintf(stderr, "plugin install failed (fingerprint 0x%08X)\n", lafi::g_lastFingerprint); return 4; }
        pass(true, opt.maxPatched, patched);
        printf("  plugin: merges %lld, cache reused %lld, rebuilt %lld, self-checks %lld, self-check failures %lld\n", lafi::g_stats.merges,
               lafi::g_stats.reused, lafi::g_stats.rebuilt, lafi::g_stats.selfChecks, lafi::g_stats.selfCheckFails);
        lafi::Remove();
    }
    if (mode == "both") {
        const bool same = orig.vertexBits == patched.vertexBits && orig.polyIdx == patched.polyIdx && orig.polyPlane == patched.polyPlane;
        double to = 0, tp = 0;
        for (int i = 0; i < orig.tiles && i < int(patched.tileMs.size()); ++i) {
            to += orig.tileMs[i];
            tp += patched.tileMs[i];
        }
        printf("RESULT merge: %d tiles compared: verts %zu/%zu polys %zu/%zu -> %s; hash orig %016llX patched %016llX\n", orig.tiles,
               orig.vertexBits.size() / 3, patched.vertexBits.size() / 3, orig.polyPlane.size() / 4, patched.polyPlane.size() / 4,
               same ? "IDENTICAL" : "DIFFERENT", (unsigned long long)FnvVec(orig.polyIdx, FnvVec(orig.vertexBits)),
               (unsigned long long)FnvVec(patched.polyIdx, FnvVec(patched.vertexBits)));
        printf("RESULT merge time for the first %d tiles: orig %.1f s, patched %.1f s, speed-up %.1fx\n", orig.tiles, to / 1000.0, tp / 1000.0,
               tp > 0 ? to / tp : 0.0);
        return same ? 0 : 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc < 4 && std::string(argv[2]) != "fingerprint" && std::string(argv[2]) != "patchcheck")) {
        printf("usage: harness <Gothic2.exe> import <list.txt> <orig|patched|both> [opts]\n"
               "       harness <Gothic2.exe> merge <zen> <meshdir> <orig|patched|both> [opts]\n"
               "       harness <Gothic2.exe> fingerprint\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    if (int code; laimg::RelaunchReserved("LAFI_CHILD", code)) return code;  // parent waits for the child
    if (!laimg::MapExe(argv[1])) return 2;
    lafi::g_diag = 0;
    const std::string test = argv[2];
    if (test == "fingerprint") {
        printf("fingerprint 0x%08X\n", lafi::CodeFingerprint(reinterpret_cast<const uint8_t*>(lafi::kImageBase)));
        return 0;
    }
    laimg::InitEngine();
    if (test == "patchcheck") {
        // every patch site: original bytes before Install, our branch after, original again after Remove;
        // the two BIN checks must become "jmp" to exactly the target of the original "jbe"
        auto bytes = [](uint32_t a, int n) { std::string r; char b[4]; for (int i = 0; i < n; ++i) { sprintf_s(b, "%02X", reinterpret_cast<uint8_t*>(a)[i]); r += b; } return r; };
        auto jccTarget = [](uint32_t a) { return a + 6 + *reinterpret_cast<int32_t*>(a + 2); };
        auto jmpTarget = [](uint32_t a) { return a + 5 + *reinterpret_cast<int32_t*>(a + 1); };
        const uint32_t sites[2] = {lafi::kSaveBinCheck, lafi::kLoadBinCheck};
        uint32_t orig[2];
        int bad = 0;
        for (int i = 0; i < 2; ++i) { orig[i] = jccTarget(sites[i]); printf("  %08X before %s jbe -> %08X\n", sites[i], bytes(sites[i], 6).c_str(), orig[i]); }
        std::vector<std::string> before;
        for (const auto& d : lafi::Patches()) before.push_back(bytes(d.address, d.size));
        if (!lafi::Install(false)) { printf("install failed, fingerprint 0x%08X\n", lafi::g_lastFingerprint); return 4; }
        for (int i = 0; i < 2; ++i) {
            const uint32_t t = jmpTarget(sites[i]);
            printf("  %08X after  %s jmp -> %08X %s\n", sites[i], bytes(sites[i], 6).c_str(), t, t == orig[i] ? "same target" : "WRONG");
            if (t != orig[i] || reinterpret_cast<uint8_t*>(sites[i])[0] != 0xE9) ++bad;
        }
        lafi::Remove();
        int k = 0;
        for (const auto& d : lafi::Patches()) if (bytes(d.address, d.size) != before[k++]) { printf("  %08X not restored\n", d.address); ++bad; }
        printf("RESULT patchcheck: %d sites, %s\n", int(before.size()), bad ? "FAILED" : "ok (patched as designed, restored by Remove)");
        return bad ? 1 : 0;
    }
    Options opt;
    int argi = (test == "merge") ? 6 : 5;
    const std::string mode = (test == "merge") ? (argc > 5 ? argv[5] : "both") : argv[4];
    for (int i = argi; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-max" && i + 1 < argc) opt.maxOrig = atoi(argv[++i]);
        else if (a == "-maxp" && i + 1 < argc) opt.maxPatched = atoi(argv[++i]);
        else if (a == "-tlimit" && i + 1 < argc) opt.tlimit = atof(argv[++i]);
        else if (a == "-selfcheck") lafi::g_selfCheck = 1;
        else if (a == "-noreuse") lafi::g_reuseMerge = 0;
        else if (a == "-noremap") lafi::g_fastRemap = 0;
        else if (a == "-nocache") lafi::g_fastCache = 0;
        else if (a == "-diag") { lafi::g_diag = 1; lafi::g_module = GetModuleHandleW(nullptr); }
    }
    if (test == "import") {
        return RunImport(ReadLines(argv[3]), mode, opt);
    }
    if (test == "vobload") {
        return RunVobLoad(ReadLines(argv[3]), mode);
    }
    if (test == "merge") {
        if (argc < 6) return 2;
        return RunMerge(TilesFromZen(argv[3], argv[4]), mode, opt);
    }
    return 2;
}
