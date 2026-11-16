# Manhunt 1 Revolver Reload Fix

An ASI plugin for Manhunt (PC, Steam) that fixes a vanilla bug: after you reload the pump shotgun
(`Shotgun_(CT)` or `Shotgun_Torch_(CT)`) with shells still in it, the 6-shooter revolver's next
reload skips its chamber-flick. The cylinder stays swung open until you fire again.

## Install

This repo is source only, with no prebuilt binary. Build it first (see Build below).

1. You need an ASI loader in the game folder (for example Ultimate ASI Loader). If other `.asi`
   plugins already work for you, you have one.
2. Copy the built `bin\RevolverFlickFix.asi` into the game's `scripts\` folder.
3. Start the game. `revolverflickfix_asi.log` appears in the game folder. A good run contains two
   `patched 0x...` lines. `ABORT:` means the exe wasn't the expected version and nothing was
   changed. To uninstall, delete the `.asi`.

It patches the game in memory only. `manhunt.exe` is never modified. Tested on the Steam version
(image base 0x400000, files dated 2022).

## The bug

Repro: reload the revolver (the flick plays). Fire and reload the shotgun while it still has
shells loaded. Switch back, fire, and reload the revolver. The reload ends with the cylinder
open. Firing snaps it shut, because the fire anim resets the pose. The sawn-off doesn't cause
it. Reloading the shotgun from empty makes the revolver work again.

## Cause

- `WeaponTypeData.ini` `RELOAD_LOOP a, b` sets weapon-data +0xA8 = 1, +0xAC = a, +0xB0 = b
  (parser at 0x442670). Only `6shooter` (1.067, 1.267) and `shotgun` (0.0, 0.367) have it.
- `CPed+0x33C` bit `0x40` means "this looped reload started with rounds still in the clip, so
  stop the anim when the loop ends". That is how a partial shotgun reload skips the pump.
- The player reload request (around 0x4B0F81) only writes that bit when the held collectable is
  `CT_SHOTGUN` (0x2C) or `CT_SHOTGUN_TORCH` (0x2D). At 0x4B1114, clip != 0 sets it and an empty
  clip clears it. Other guns start their reload at 0x4B1130 and leave the bit alone, as does the
  empty-clip reload at 0x45F479.
- `SF_TorsoReload` (0x4B86E0) checks the bit for **any** RELOAD_LOOP weapon when it leaves the
  loop (0x4B89F9). If it's set, it cuts the anim and calls 0x425410 to return to idle. For the
  revolver, that cut lands at 1.267 s.
- In `6Shooter_Reload_W` the cylinder swings out at 0.28–0.63 s, stays open until 1.90 s, and is
  shut again at 1.96 s. A cut at 1.267 s therefore leaves it open.
- The bit isn't cleared on weapon switch. It's only cleared by an empty-shotgun reload, the cock
  re-reload (0x4B8862), and two other paths (0x4AC440, 0x4B9020).

It isn't the animation data: the `6Shooter_*`, `Shotgun_*` and `SawnOff_*` animations are
identical across all the level `allanims.ifp` files, and the logic is hard-coded in the exe.

The 6-shooter is the only victim in vanilla data because it's the only other RELOAD_LOOP weapon.
A mod that gives another weapon `RELOAD_LOOP` would be affected the same way.

Manhunt 2 has the same bug at different addresses. This repo only covers Manhunt 1.

## The fix

At both non-shotgun reload starts (0x4B1130 and 0x45F479), the 11 bytes
`mov ecx,[ebp+2FCh]; call 0x425410` are replaced with a jump to a stub. The stub runs
`and dword [ebp+33Ch], 0FFFFFFBFh`, then the original two instructions, then jumps back. The
shotgun's own path is untouched, so partial shotgun reloads still skip the pump as in vanilla.
Before patching, the plugin checks the image base and the original bytes at both sites, and does
nothing if they differ.

## Build

`build.cmd` builds `bin\RevolverFlickFix.asi` with Visual Studio's `cl` (Win32). Nothing else is
needed. Edit the `VCVARS` line in the script if your Visual Studio is in a different place.

## Confirmed in game

After the fix, the revolver flick plays on every reload following a `Shotgun_(CT)` and a
`Shotgun_Torch_(CT)` reload.
