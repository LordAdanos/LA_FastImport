// la_exe_image.h - test-harness loader for the LA_* Union plugins (MIT, Copyright (c) 2026 LordAdanos; see LICENSE).
// Generic loader: maps a Gothic2.exe so a test can call the engine's own machine code without starting the game.
//
// Maps the Steam Gothic2.exe (2.6 fix, read-only file access) at 0x400000 into a 32-bit test process,
// resolves its imports (system DLLs only from System32; SmartHeap -> a private heap; game DLLs such as
// ddraw/dinput/mss32/binkw32/vdfs32g/BugslayerUtil -> stubs that are never loaded and abort on use),
// runs the exe's CRT init and static initializers, and lets a test call the ENGINE'S OWN machine code.
// Gothic is never started: no window, no GPU, no game files besides Gothic2.exe are touched.
//
// Usage in a harness (one .cpp, built /MT /LARGEADDRESSAWARE /FIXED /BASE:0x20000000):
//   int main(int argc, char** argv) {
//       if (int code; laimg::RelaunchReserved("LA_CHILD", code)) return code;   // parent: runs the child
//       if (!laimg::MapExe(argv[1])) return 2;
//       laimg::InitEngine();
//       ... call engine functions ...
//   }
// Pitfalls (learned the hard way): 0x400000 is often taken by the process heap, therefore the
// process restarts itself suspended and reserves the range in the child before its loader runs;
// set the zerr level byte to 0 after init so the engine's log paths stay quiet; anything that needs
// VDFS/file IO through the engine hits a stub and aborts (feed data yourself).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

namespace laimg {

constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kReserveSize = 0x00752000;  // SizeOfImage of the Steam 2.6 fix exe

// ---------------------------------------------------------------------------------- heap / stubs
inline HANDLE& Heap() {
    static HANDLE heap = nullptr;
    if (!heap) heap = HeapCreate(0, 0, 0);
    return heap;
}
inline void* __cdecl H_malloc(size_t n) { return HeapAlloc(Heap(), 0, n ? n : 1); }
inline void __cdecl H_free(void* p) {
    if (p) HeapFree(Heap(), 0, p);
}
inline void* __cdecl H_realloc(void* p, size_t n) {
    if (!p) return H_malloc(n);
    if (!n) {
        H_free(p);
        return nullptr;
    }
    return HeapReAlloc(Heap(), 0, p, n);
}
inline void* __cdecl H_calloc(size_t a, size_t b) { return HeapAlloc(Heap(), HEAP_ZERO_MEMORY, a * b ? a * b : 1); }
inline size_t __cdecl H_msize(void* p) { return p ? HeapSize(Heap(), 0, p) : 0; }
inline int __cdecl H_zero() { return 0; }
inline void* __cdecl H_null() { return nullptr; }

inline std::vector<std::string>& StubNames() {
    static std::vector<std::string> names;
    return names;
}
extern "C" inline void __cdecl HsImgStubHit(int index) {
    const auto& n = StubNames();
    fprintf(stderr, "\nFATAL: engine called stubbed import %s\n", index >= 0 && index < int(n.size()) ? n[index].c_str() : "?");
    fflush(stderr);
    fflush(stdout);
    ExitProcess(3);
}
inline void* MakeStub(const std::string& name) {
    static uint8_t* thunks = nullptr;
    static int count = 0;
    if (!thunks) thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    const int index = int(StubNames().size());
    StubNames().push_back(name);
    uint8_t* t = thunks + 16 * count++;
    t[0] = 0x68;  // push index
    memcpy(t + 1, &index, 4);
    t[5] = 0xE8;  // call HsImgStubHit
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&HsImgStubHit)) - int32_t(reinterpret_cast<uintptr_t>(t + 10));
    memcpy(t + 6, &rel, 4);
    t[10] = 0xCC;
    return t;
}

inline bool ReadFileAll(const char* path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const size_t got = n > 0 ? fread(out.data(), 1, size_t(n), f) : 0;
    fclose(f);
    return got == out.size();
}

// The parent restarts itself suspended with env var `marker` set, reserves 0x400000.. in the child,
// resumes it and returns true with the child's exit code. In the child it returns false.
inline bool RelaunchReserved(const char* marker, int& exitCode) {
    if (GetEnvironmentVariableA(marker, nullptr, 0)) return false;
    SetEnvironmentVariableA(marker, "1");
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, GetCommandLineA(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed %lu\n", GetLastError());
        exitCode = 2;
        return true;
    }
    if (!VirtualAllocEx(pi.hProcess, reinterpret_cast<void*>(kImageBase), kReserveSize, MEM_RESERVE, PAGE_NOACCESS)) {
        fprintf(stderr, "reserve in child failed %lu\n", GetLastError());
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    exitCode = int(code);
    return true;
}

inline uint32_t& ExeFileSize() {
    static uint32_t size = 0;
    return size;
}

inline bool MapExe(const char* path) {
    std::vector<uint8_t> file;
    if (!ReadFileAll(path, file) || file.size() < 0x400) {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    ExeFileSize() = uint32_t(file.size());
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.ImageBase != kImageBase) {
        fprintf(stderr, "unexpected image base\n");
        return false;
    }
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(kImageBase), imageSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!base) base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(kImageBase), imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (base != reinterpret_cast<uint8_t*>(kImageBase)) {
        fprintf(stderr, "0x400000 not free (err %lu)\n", GetLastError());
        return false;
    }
    memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint32_t raw = (std::min)(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if (raw) memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, raw);
    }
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    const char* systemDlls[] = {"KERNEL32.dll", "USER32.dll", "GDI32.dll", "ADVAPI32.dll", "WINMM.dll", "ole32.dll", "COMCTL32.dll", "WSOCK32.dll"};
    for (; imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        HMODULE mod = nullptr;
        for (const char* s : systemDlls) {
            if (_stricmp(s, dll) == 0) {
                char sys[MAX_PATH];
                GetSystemDirectoryA(sys, MAX_PATH);
                strcat_s(sys, "\\");
                strcat_s(sys, dll);
                mod = LoadLibraryA(sys);  // only from System32
            }
        }
        const bool smartHeap = _stricmp(dll, "SHW32.DLL") == 0;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + imp->FirstThunk);
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (int k = 0; names[k].u1.AddressOfData; ++k) {
            std::string name;
            FARPROC fn = nullptr;
            if (names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
                name = std::string(dll) + "#" + std::to_string(names[k].u1.Ordinal & 0xFFFF);
                if (mod) fn = GetProcAddress(mod, reinterpret_cast<LPCSTR>(uintptr_t(names[k].u1.Ordinal & 0xFFFF)));
            } else {
                const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[k].u1.AddressOfData);
                name = std::string(dll) + "!" + reinterpret_cast<const char*>(ibn->Name);
                if (mod) fn = GetProcAddress(mod, reinterpret_cast<const char*>(ibn->Name));
            }
            void* target = reinterpret_cast<void*>(fn);
            if (smartHeap) {
                // IAT slots of the SmartHeap imports (import thunks 0x7B449C..0x7B44FC)
                const uint32_t slot = uint32_t(reinterpret_cast<uintptr_t>(&thunk[k]));
                switch (slot) {
                    case 0x0082E2F0: target = reinterpret_cast<void*>(&H_calloc); break;
                    case 0x0082E308: target = reinterpret_cast<void*>(&H_free); break;
                    case 0x0082E30C: target = reinterpret_cast<void*>(&H_malloc); break;
                    case 0x0082E314: target = reinterpret_cast<void*>(&H_realloc); break;
                    case 0x0082E31C: target = reinterpret_cast<void*>(&H_msize); break;
                    case 0x0082E328: target = reinterpret_cast<void*>(&H_null); break;  // shi_expand: "cannot"
                    default: target = reinterpret_cast<void*>(&H_zero); break;          // heapchk/pool init...
                }
            }
            if (!target) target = MakeStub(name);
            thunk[k].u1.Function = uint32_t(reinterpret_cast<uintptr_t>(target));
        }
    }
    return true;
}

inline int& InitFailures() {
    static int n = 0;
    return n;
}
inline bool CallInit(uint32_t address, const char* what) {
    __try {
        reinterpret_cast<void(__cdecl*)()>(address)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "init %s at 0x%08X raised 0x%08lX\n", what, address, GetExceptionCode());
        ++InitFailures();
        return false;
    }
}
inline int CallHeapInit() {
    __try {
        return reinterpret_cast<int(__cdecl*)(int)>(0x007DBA96)(1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// _WinMainCRTStartup without WinMain: _heap_init, _mtinit, _ioinit, then each _initterm entry.
inline int InitEngine(bool verbose = true) {
    const int heap = CallHeapInit();
    CallInit(0x007D622C, "_mtinit");
    CallInit(0x007D4D6B, "_ioinit");
    int ok = 0, total = 0;
    auto run = [&](uint32_t from, uint32_t to) {
        for (uint32_t p = from; p < to; p += 4) {
            const uint32_t fn = *reinterpret_cast<uint32_t*>(p);
            if (!fn) continue;
            ++total;
            char what[32];
            sprintf_s(what, "initterm[%08X]", p);
            if (CallInit(fn, what)) ++ok;
        }
    };
    run(0x0088DBC8, 0x0088DBDC);  // C initializers (__xi)
    run(0x0088C000, 0x0088DBC4);  // C++ static constructors (__xc)
    *reinterpret_cast<int8_t*>(0x008CDCF0) = 0;  // zerr log level: no engine log output
    if (verbose) printf("crt: heap_init=%d, %d of %d static initializers ok\n", heap, ok, total);
    return total - ok;
}

}  // namespace laimg
