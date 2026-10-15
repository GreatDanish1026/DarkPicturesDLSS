// Finds Unreal console variables in the running game and returns their data
// (TConsoleVariableData<T>: two copies of the value, [0] game thread, [1] render
// thread), so the probe can force them every frame.
//
// IConsoleManager::Singleton is found from the game's own code: every cvar
// lookup loads it with `mov rcx, [rip+Singleton]` shortly before
// `lea rdx, [rip+L"r.ScreenPercentage"]`. Vtable offsets are UE 4.26's, as seen
// in The Quarry (build 11105655):
//   IConsoleManager +0xb0  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//   IConsoleVariable +0x58 AsVariableInt()    -> TConsoleVariableData<int32>*
//   IConsoleVariable +0x60 AsVariableFloat()  -> TConsoleVariableData<float>*
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb0;
constexpr uint32_t kAsVariableInt = 0x58;
constexpr uint32_t kAsVariableFloat = 0x60;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

// IConsoleManager::Singleton (the variable holding the pointer), or null.
inline void** FindConsoleManagerSingleton(const uint8_t* base) {
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return nullptr;
    const uint8_t* name = FindWideString(rdata, L"r.ScreenPercentage");
    if (!name) return nullptr;
    for (size_t i = 0x40; i + 7 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea r64, [rip+disp32] -> the name
        if ((p[0] != 0x48 && p[0] != 0x4c) || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != name) continue;
        // mov rcx, [rip+disp32] within the 0x40 bytes before it
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            return reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp));
        }
    }
    return nullptr;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using AsDataFn = void*(__fastcall*)(void* self);

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// The data of console variable <name>, or null. Guarded: a wrong vtable layout
// faults here instead of taking the game down.
inline void* FindData(void* manager, const wchar_t* name, bool isFloat) {
    __try {
        void* var = Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, true);
        if (!var) return nullptr;
        return Virtual<AsDataFn>(var, isFloat ? kAsVariableFloat : kAsVariableInt)(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace cvars
