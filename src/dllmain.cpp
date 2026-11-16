// RevolverFlickFix.asi - Manhunt 1 PC (Steam). Fixes the 6-shooter reload skipping its
// chamber-flick after the pump shotgun (Shotgun_(CT) / Shotgun_Torch_(CT)) has been reloaded.
//
// Cause (vanilla engine bug, also present in Manhunt 2):
//   CPed+0x33C bit 0x40 means "this looped reload started with rounds still loaded, so when the
//   loop finishes, stop the reload anim instead of playing its tail". It exists so a partial
//   shotgun reload ends without the pump (Shotgun_Cock).
//   - The player reload request (0x4B0F81..) sets/clears the bit ONLY when the held collectable is
//     CT_SHOTGUN (0x2C) or CT_SHOTGUN_TORCH (0x2D) (0x4B1114: clip != 0 -> set, else clear).
//   - Every other gun starts its reload at 0x4B1130 without touching the bit, and the empty-clip
//     reload at 0x45F479 doesn't touch it either.
//   - SF_TorsoReload (0x4B86E0) checks the bit when ANY RELOAD_LOOP weapon leaves its loop
//     (0x4B89F9): set -> anim cut at the loop end and the torso FSM sent back to idle (0x425710).
//   The 6-shooter is the only other RELOAD_LOOP weapon (RELOAD_LOOP 1.067, 1.267) and its
//   chamber-flick is the tail after 1.267, so after one partial shotgun reload every revolver
//   reload stops with the cylinder open, until the bit is cleared by an empty shotgun reload,
//   death/weapon-drop paths (0x4AC440, 0x4B9020), etc. Firing plays the fire anim, which
//   resets the model's pose, which is why the cylinder snaps shut on the next shot.
//
// Fix: at both non-shotgun reload starts, clear bit 0x40 before calling SetReload, as the
// shotgun path already does in effect. The shotgun's own behaviour is unchanged.
//
//   0x4B1130  8B 8D FC 02 00 00   mov ecx,[ebp+2FCh]      ; torso FSM
//   0x4B1136  E8 rel32            call CPedTorsoAnimFSM::SetReload (0x425410)
//   (same 11 bytes at 0x45F479)
// Each site becomes  jmp stub / nop x6, stub = and dword [ebp+33Ch],0FFFFFFBFh ; original 11
// bytes (call re-targeted) ; jmp site+11.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

namespace
{
    constexpr uintptr_t EXPECTED_IMAGE_BASE = 0x400000;
    constexpr uintptr_t ADDR_SET_RELOAD = 0x425410;   // CPedTorsoAnimFSM::SetReload
    constexpr uintptr_t SITES[] = { 0x4B1130, 0x45F479 };
    constexpr int SITE_LEN = 11;

    void Log(const char* fmt, ...)
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\'))
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "revolverflickfix_asi.log");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") != 0 || !f) return;
        va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
        fputc('\n', f); fclose(f);
    }

    int32_t Rel(uintptr_t from, uintptr_t to) { return static_cast<int32_t>(to - (from + 5)); }

    bool SiteMatches(uintptr_t site)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(site);
        static const unsigned char MOV[6] = { 0x8B, 0x8D, 0xFC, 0x02, 0x00, 0x00 };
        if (memcmp(p, MOV, 6) != 0 || p[6] != 0xE8) return false;
        int32_t rel; memcpy(&rel, p + 7, 4);
        return site + 11 + rel == ADDR_SET_RELOAD;
    }

    bool PatchSite(uintptr_t site, unsigned char* stub)
    {
        // stub
        unsigned char* s = stub;
        const unsigned char AND40[7] = { 0x83, 0xA5, 0x3C, 0x03, 0x00, 0x00, 0xBF }; // and dword [ebp+33Ch],-41h
        memcpy(s, AND40, 7); s += 7;
        const unsigned char MOV[6] = { 0x8B, 0x8D, 0xFC, 0x02, 0x00, 0x00 };          // mov ecx,[ebp+2FCh]
        memcpy(s, MOV, 6); s += 6;
        int32_t r = Rel(reinterpret_cast<uintptr_t>(s), ADDR_SET_RELOAD);
        *s++ = 0xE8; memcpy(s, &r, 4); s += 4;                                         // call SetReload
        r = Rel(reinterpret_cast<uintptr_t>(s), site + SITE_LEN);
        *s++ = 0xE9; memcpy(s, &r, 4); s += 4;                                         // jmp back

        // site
        unsigned char buf[SITE_LEN];
        buf[0] = 0xE9;
        r = Rel(site, reinterpret_cast<uintptr_t>(stub));
        memcpy(buf + 1, &r, 4);
        memset(buf + 5, 0x90, SITE_LEN - 5);
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), SITE_LEN, PAGE_EXECUTE_READWRITE, &prot)) return false;
        memcpy(reinterpret_cast<void*>(site), buf, SITE_LEN);
        VirtualProtect(reinterpret_cast<void*>(site), SITE_LEN, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), SITE_LEN);
        return true;
    }

    void Install()
    {
        Log("RevolverFlickFix: loading");
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (base != EXPECTED_IMAGE_BASE) { Log("  ABORT: image base 0x%08X, expected 0x400000.", static_cast<unsigned>(base)); return; }
        for (uintptr_t site : SITES)
            if (!SiteMatches(site)) { Log("  ABORT: bytes at 0x%08X are not the expected reload call; nothing patched.", static_cast<unsigned>(site)); return; }

        unsigned char* mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!mem) { Log("  ABORT: VirtualAlloc failed."); return; }
        int i = 0;
        for (uintptr_t site : SITES)
        {
            if (PatchSite(site, mem + i * 32)) Log("  patched 0x%08X -> stub %p (clears CPed+0x33C bit 0x40 before SetReload)", static_cast<unsigned>(site), mem + i * 32);
            else Log("  FAILED to patch 0x%08X", static_cast<unsigned>(site));
            ++i;
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hModule); Install(); }
    return TRUE;
}
