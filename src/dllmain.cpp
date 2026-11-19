// ReloadFixes.asi - Manhunt 1 PC (Steam). Fixes four vanilla reload bugs. Each one can be turned
// on or off in ReloadFixes.ini (next to the .asi). All are on (and logging off) if the .ini is missing.
//
// The revolver and shotgun bugs come from the same per-ped flag: CPed+0x33C bit 0x40, "this looped reload started
// with rounds still loaded, so cut the reload anim when RELOAD_LOOP ends". It lets a partial
// shotgun reload skip the pump. Only the normal shotgun reload start writes it (0x4B1114: clip
// != 0 -> set, empty -> clear), but the torso reload state (0x4B89F9) obeys it for every
// RELOAD_LOOP weapon.
//
// [Fixes] RevolverFlick
//   The other guns start their reload at 0x4B1130 (and the empty-clip reload at 0x45F479)
//   without touching the bit. After a partial shotgun reload the bit stays set, so the
//   6-shooter's reload (RELOAD_LOOP 1.067, 1.267) is cut before its chamber-flick and the
//   cylinder stays open until the next shot.
//   Fix: clear the bit before SetReload at both sites.
//
// [Fixes] SniperLastCasing
//   The sniper's case is ejected by the bolt cycle (Sniper_Cock, SHELL_EJECT_DELAY_FIRE 0.2,
//   eject at 0x4B88DD). After the last shot, the same cock state first sees the empty magazine
//   and calls SetReload (0x4B885D) on every frame instead of reaching the eject point, so that
//   case is never ejected. The cock anim still plays out; only the eject is skipped.
//   Fix: for the sniper rifle, skip that SetReload until the bolt is closed again (0.55 s,
//   Sniper_Cock_T closes it at 0.491 s) and continue into the normal bolt-cycle code
//   (0x4B8880), which ejects the case at 0.2 s exactly like after any other shot. From then on,
//   call SetReload as vanilla does but still run the bolt-cycle code, which keeps the rifle's
//   model anim (Sniper_Cock_W) synced to Cash's hands while the reload blends in. Without that
//   sync the rifle froze mid-bolt and dropped out of his hands. Cost: the reload after an empty
//   magazine starts about 0.5 s later than vanilla, after the bolt is worked.
//
// [Fixes] ShotgunLastShell
//   The pump shotgun has the same skipped eject: after the last shot the pump (Shotgun_Cock_T,
//   SHELL_EJECT_DELAY_FIRE 0.169) plays without ejecting, because the same code (0x4B885D)
//   requests the reload first. Vanilla works around it with SHELL_EJECT_DELAY_RELOAD 0.7, which
//   ejects the shell late in a reload from empty, right before the closing pump.
//   Fix: the same deferral as SniperLastCasing for CT_SHOTGUN / CT_SHOTGUN_TORCH (released at
//   0.35 s, after the pump closes at 0.313 s), so the shell comes out on the pump after the
//   last shot. And during the reload, don't re-arm the "shell already ejected" bit (0x4B8BD7),
//   so the 0.7 s reload eject only happens if that shell is still in the gun.
//
// [Fixes] ShotgunQueuedReload
//   Pressing reload while a shotgun shot or pump is playing doesn't start the reload. It sets
//   CPed+0x33C bit 0x80 (0x4B10F1), and TorsoReloadCallback (0x4BA0D0) starts it when the pump
//   ends (0x4BA11E) without writing bit 0x40. The queued reload then uses the value left by the
//   previous reload: after a reload from empty, a partial reload ends with a pump that also
//   ejects a phantom shell.
//   Fix: after the queued SetReload, set or clear the bit from the shells loaded, as the normal
//   start does.
//
// Every patch checks the original bytes first and does nothing if they differ.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

namespace
{
    constexpr uintptr_t EXPECTED_IMAGE_BASE = 0x400000;
    constexpr uintptr_t ADDR_SET_RELOAD = 0x425410;   // CPedTorsoAnimFSM::SetReload

    // RevolverFlick, non-shotgun reload starts: mov ecx,[ebp+2FCh] ; call SetReload  (11 bytes), ebp = ped
    constexpr uintptr_t SITE_RELOAD_START  = 0x4B1130;
    constexpr uintptr_t SITE_EMPTY_RELOAD  = 0x45F479;

    // ShotgunQueuedReload site: mov ecx,esi ; call SetReload  (7 bytes), ebx = ped
    constexpr uintptr_t SITE_QUEUED_RELOAD = 0x4BA11C;

    // SniperLastCasing, auto reload in the bolt-cycle state: mov ecx,[esi+2FCh] ; call SetReload
    // (11 bytes), esi = ped. 0x4B8880 is where that code continues when rounds are loaded.
    constexpr uintptr_t SITE_BOLT_AUTO_RELOAD = 0x4B8857;

    // ShotgunLastShell, reload-eject re-arm: mov eax,[esp+3Ch] ; and dword [eax+13Ch],-11h
    // (11 bytes), reached only while the reload anim is before SHELL_EJECT_DELAY_RELOAD.
    constexpr uintptr_t SITE_RELOAD_EJECT_REARM = 0x4B8BD7;

    bool g_log = true;
    char g_dir[MAX_PATH] = {};

    void Log(bool always, const char* fmt, ...)
    {
        if (!g_log && !always) return;
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\'))
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "reloadfixes_asi.log");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") != 0 || !f) return;
        va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
        fputc('\n', f); fclose(f);
    }

    // The naked stubs below use literal addresses: MSVC inline asm reads a C++ constant name as a
    // memory operand, not an immediate. Keep them in sync with the SITE_ values above.

    bool g_revolverFlick = true;
    bool g_sniperLastCasing = true;
    bool g_shotgunLastShell = true;

    constexpr int CT_SHOTGUN       = 0x2C;
    constexpr int CT_SHOTGUN_TORCH = 0x2D;
    constexpr int CT_SNIPER_RIFLE  = 0x30;    // also CT_SNIPER_RIFLE_SILENCED
    // Sniper_Cock_T: bolt back at 0.267 s, bolt forward at 0.491 s (anim events 0x119/0x11A).
    constexpr float BOLT_CLOSED_TIME = 0.55f;
    // Shotgun_Cock_T / Shotgun_ZoomCock_T: pump back at 0.169 s, forward at 0.313 s.
    constexpr float PUMP_CLOSED_TIME = 0.35f;

    // ped -> held collectable (+0x150 inventory, +0x154 slot) and its ammo weapon (+0x1AC).
    // Returns false if anything is missing.
    bool HeldWeapon(uintptr_t ped, uintptr_t& coll, uintptr_t& weapon)
    {
        const int slot = *reinterpret_cast<int*>(ped + 0x154);
        if (slot < 0 || slot > 64) return false;
        const uintptr_t inv = *reinterpret_cast<uintptr_t*>(ped + 0x150);
        if (!inv) return false;
        const uintptr_t items = *reinterpret_cast<uintptr_t*>(inv + 8);
        if (!items) return false;
        coll = *reinterpret_cast<uintptr_t*>(items + slot * 4);
        if (!coll) return false;
        weapon = *reinterpret_cast<uintptr_t*>(coll + 0x1AC);
        return weapon != 0;
    }

    // ---- ShotgunQueuedReload: set bit 0x40 from the held weapon's loaded rounds ----
    void __cdecl SetPartialReloadFlag(uintptr_t ped)
    {
        __try
        {
            uintptr_t coll, weapon;
            if (!HeldWeapon(ped, coll, weapon)) return;
            const uint32_t loaded = *reinterpret_cast<uint32_t*>(weapon + 0xA0);
            uint32_t& flags = *reinterpret_cast<uint32_t*>(ped + 0x33C);
            if (loaded != 0) flags |= 0x40; else flags &= ~0x40u;   // same rule as 0x4B1114
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- SniperLastCasing: 1 = skip this frame's auto reload so the bolt can eject the case ----
    // Returns 0: vanilla (SetReload, return); 1: skip SetReload and run the bolt-cycle code;
    // 2: SetReload, clear CPed+0x33C bits 0x40/0x80 as vanilla does, then run the bolt-cycle code.
    int __cdecl DeferLastRoundAutoReload(uintptr_t ped)
    {
        __try
        {
            uintptr_t coll, weapon;
            if (!HeldWeapon(ped, coll, weapon)) return 0;
            const uintptr_t typeData = *reinterpret_cast<uintptr_t*>(coll + 0x7C);
            if (!typeData) return 0;
            const int ct = *reinterpret_cast<int*>(typeData + 0x164);
            float closedTime;
            if (ct == CT_SNIPER_RIFLE && g_sniperLastCasing) closedTime = BOLT_CLOSED_TIME;
            else if ((ct == CT_SHOTGUN || ct == CT_SHOTGUN_TORCH) && g_shotgunLastShell) closedTime = PUMP_CLOSED_TIME;
            else return 0;
            if (*reinterpret_cast<float*>(typeData + 0x33C) <= 0.0f) return 0;     // no bolt eject time
            const uintptr_t fsm = *reinterpret_cast<uintptr_t*>(ped + 0x2FC);
            if (!fsm) return 0;
            const uintptr_t anim = *reinterpret_cast<uintptr_t*>(fsm + 0x94);
            if (!anim) return 0;
            // Sniper bolt or shotgun pump. Until the bolt is closed again, play the bolt cycle (which ejects the case at 0.2 s).
            // After that, request the reload but keep running the bolt-cycle code, which keeps the
            // rifle's model anim synced to Cash's hands while the reload blends in.
            return *reinterpret_cast<float*>(anim + 0x20) < closedTime ? 1 : 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // ---- Non-shotgun reload start (0x4B1130 and 0x45F479), before SetReload ----
    //   RevolverFlick: clear the stale bit 0x40.
    void __cdecl OnReloadStart(uintptr_t ped)
    {
        __try
        {
            *reinterpret_cast<uint32_t*>(ped + 0x33C) &= ~0x40u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 0x4B8857: mov ecx,[esi+2FCh] ; call SetReload  (11 bytes), esi = ped
    __declspec(naked) void StubBoltAutoReload()
    {
        __asm {
            push esi                        // ped
            call DeferLastRoundAutoReload
            add esp, 4
            cmp eax, 1
            je bolt
            mov ecx, dword ptr [esi + 0x2FC]
            mov ebx, eax                    // ebx is reloaded by the bolt-cycle code (0x4B8891)
            mov eax, 0x425410               // CPedTorsoAnimFSM::SetReload
            call eax
            test ebx, ebx
            jnz release
            push 0x4B8862                   // vanilla: SITE_BOLT_AUTO_RELOAD + 11
            ret
        release:
            and dword ptr [esi + 0x33C], 0xFFFFFF3F    // as vanilla does after this SetReload
        bolt:
            push 0x4B8880                   // bolt-cycle code, as when rounds are loaded
            ret
        }
    }

    // ---- ShotgunLastShell: reload-eject re-arm (0x4B8BD7) ----
    //   Vanilla clears the collectable's "shell already ejected" bit (+0x13C & 0x10) on every
    //   reload frame before SHELL_EJECT_DELAY_RELOAD, so the reload always ejects one. For the
    //   pump shotguns, keep the bit: if the pump after the last shot already ejected the shell,
    //   the reload doesn't eject a second one; if not, the bit is still clear (firing clears it,
    //   0x4C9CB3) and the reload ejects it as in vanilla.
    __declspec(naked) void StubReloadEjectRearm()
    {
        __asm {
            mov eax, dword ptr [esp + 0x3C]     // original: collectable
            mov ecx, dword ptr [eax + 0x7C]     // type data (ecx is rewritten before use after 0x4B8BE2)
            test ecx, ecx
            jz rearm
            mov ecx, dword ptr [ecx + 0x164]    // collectable type
            cmp ecx, 0x2C                       // CT_SHOTGUN
            je keep
            cmp ecx, 0x2D                       // CT_SHOTGUN_TORCH
            je keep
        rearm:
            and dword ptr [eax + 0x13C], 0xFFFFFFEF
        keep:
            push 0x4B8BE2                       // SITE_RELOAD_EJECT_REARM + 11
            ret
        }
    }

    __declspec(naked) void StubQueuedReload()
    {
        __asm {
            mov ecx, esi                    // original: mov ecx,esi ; call SetReload
            mov eax, 0x425410               // CPedTorsoAnimFSM::SetReload
            call eax
            push ebx                        // ped
            call SetPartialReloadFlag
            add esp, 4
            push 0x4BA123                   // SITE_QUEUED_RELOAD + 7
            ret
        }
    }

    // ---- Non-shotgun reload starts: OnReloadStart(ped), then the original instructions ----
    __declspec(naked) void StubReloadStart()
    {
        __asm {
            push ebp                        // ped
            call OnReloadStart
            add esp, 4
            mov ecx, dword ptr [ebp + 0x2FC]
            mov eax, 0x425410               // CPedTorsoAnimFSM::SetReload
            call eax
            push 0x4B113B                   // SITE_RELOAD_START + 11
            ret
        }
    }

    __declspec(naked) void StubEmptyReload()
    {
        __asm {
            push ebp                        // ped
            call OnReloadStart
            add esp, 4
            mov ecx, dword ptr [ebp + 0x2FC]
            mov eax, 0x425410               // CPedTorsoAnimFSM::SetReload
            call eax
            push 0x45F484                   // SITE_EMPTY_RELOAD + 11
            ret
        }
    }

    // ---- patching helpers ----
    bool CallsSetReload(uintptr_t callInsn)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(callInsn);
        if (p[0] != 0xE8) return false;
        int32_t rel; memcpy(&rel, p + 1, 4);
        return callInsn + 5 + rel == ADDR_SET_RELOAD;
    }

    bool MatchMovEcxFsm(uintptr_t site)        // mov ecx,[ebp+2FCh] ; call SetReload
    {
        static const unsigned char MOV[6] = { 0x8B, 0x8D, 0xFC, 0x02, 0x00, 0x00 };
        return memcmp(reinterpret_cast<const void*>(site), MOV, 6) == 0 && CallsSetReload(site + 6);
    }

    bool MatchMovEcxFsmEsi(uintptr_t site)     // mov ecx,[esi+2FCh] ; call SetReload
    {
        static const unsigned char MOV[6] = { 0x8B, 0x8E, 0xFC, 0x02, 0x00, 0x00 };
        return memcmp(reinterpret_cast<const void*>(site), MOV, 6) == 0 && CallsSetReload(site + 6);
    }

    bool MatchReloadEjectRearm(uintptr_t site) // mov eax,[esp+3Ch] ; and dword [eax+13Ch],-11h
    {
        static const unsigned char CODE[11] = { 0x8B, 0x44, 0x24, 0x3C, 0x83, 0xA0, 0x3C, 0x01, 0x00, 0x00, 0xEF };
        return memcmp(reinterpret_cast<const void*>(site), CODE, 11) == 0;
    }

    bool MatchMovEcxEsi(uintptr_t site)        // mov ecx,esi ; call SetReload
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(site);
        return p[0] == 0x89 && p[1] == 0xF1 && CallsSetReload(site + 2);
    }

    bool WriteJump(uintptr_t site, int len, void* target)
    {
        unsigned char buf[16];
        buf[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
        memcpy(buf + 1, &rel, 4);
        memset(buf + 5, 0x90, len - 5);
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), len, PAGE_EXECUTE_READWRITE, &prot)) return false;
        memcpy(reinterpret_cast<void*>(site), buf, len);
        VirtualProtect(reinterpret_cast<void*>(site), len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), len);
        return true;
    }

    bool ReadOption(const char* key, bool def)
    {
        char ini[MAX_PATH];
        sprintf_s(ini, "%sReloadFixes.ini", g_dir);
        return GetPrivateProfileIntA(key[0] == 'L' ? "General" : "Fixes", key, def ? 1 : 0, ini) != 0;
    }

    void Install()
    {
        g_log = ReadOption("Log", false);
        g_revolverFlick    = ReadOption("RevolverFlick", true);
        g_sniperLastCasing = ReadOption("SniperLastCasing", true);
        g_shotgunLastShell = ReadOption("ShotgunLastShell", true);
        const bool queued  = ReadOption("ShotgunQueuedReload", true);
        Log(false, "ReloadFixes: loading (RevolverFlick=%d, ShotgunQueuedReload=%d, SniperLastCasing=%d, ShotgunLastShell=%d)",
            g_revolverFlick, queued, g_sniperLastCasing, g_shotgunLastShell);

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (base != EXPECTED_IMAGE_BASE)
        {
            Log(true, "ReloadFixes: ABORT: image base 0x%08X, expected 0x400000. Nothing patched.", static_cast<unsigned>(base));
            return;
        }

        if (g_revolverFlick)
        {
            if (!MatchMovEcxFsm(SITE_RELOAD_START) || !MatchMovEcxFsm(SITE_EMPTY_RELOAD))
                Log(true, "ReloadFixes: RevolverFlick ABORT: bytes at 0x%08X/0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_RELOAD_START), static_cast<unsigned>(SITE_EMPTY_RELOAD));
            else if (WriteJump(SITE_RELOAD_START, 11, StubReloadStart) && WriteJump(SITE_EMPTY_RELOAD, 11, StubEmptyReload))
                Log(false, "  RevolverFlick: patched 0x%08X and 0x%08X",
                    static_cast<unsigned>(SITE_RELOAD_START), static_cast<unsigned>(SITE_EMPTY_RELOAD));
            else
                Log(true, "ReloadFixes: RevolverFlick FAILED to write the patch.");
        }

        if (queued)
        {
            if (!MatchMovEcxEsi(SITE_QUEUED_RELOAD))
                Log(true, "ReloadFixes: ShotgunQueuedReload ABORT: bytes at 0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_QUEUED_RELOAD));
            else if (WriteJump(SITE_QUEUED_RELOAD, 7, StubQueuedReload))
                Log(false, "  ShotgunQueuedReload: patched 0x%08X", static_cast<unsigned>(SITE_QUEUED_RELOAD));
            else
                Log(true, "ReloadFixes: ShotgunQueuedReload FAILED to write the patch.");
        }

        // SniperLastCasing and ShotgunLastShell share the auto-reload hook; each is checked at run
        // time in DeferLastRoundAutoReload.
        if (g_sniperLastCasing || g_shotgunLastShell)
        {
            if (!MatchMovEcxFsmEsi(SITE_BOLT_AUTO_RELOAD))
                Log(true, "ReloadFixes: SniperLastCasing/ShotgunLastShell ABORT: bytes at 0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_BOLT_AUTO_RELOAD));
            else if (WriteJump(SITE_BOLT_AUTO_RELOAD, 11, StubBoltAutoReload))
                Log(false, "  SniperLastCasing/ShotgunLastShell: patched 0x%08X", static_cast<unsigned>(SITE_BOLT_AUTO_RELOAD));
            else
                Log(true, "ReloadFixes: SniperLastCasing/ShotgunLastShell FAILED to write the auto reload patch.");
        }

        if (g_shotgunLastShell)
        {
            if (!MatchReloadEjectRearm(SITE_RELOAD_EJECT_REARM))
                Log(true, "ReloadFixes: ShotgunLastShell ABORT: bytes at 0x%08X are not the expected re-arm; not patched.",
                    static_cast<unsigned>(SITE_RELOAD_EJECT_REARM));
            else if (WriteJump(SITE_RELOAD_EJECT_REARM, 11, StubReloadEjectRearm))
                Log(false, "  ShotgunLastShell: patched 0x%08X", static_cast<unsigned>(SITE_RELOAD_EJECT_REARM));
            else
                Log(true, "ReloadFixes: ShotgunLastShell FAILED to write the re-arm patch.");
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) slash[1] = '\0';
        Install();
    }
    return TRUE;
}
