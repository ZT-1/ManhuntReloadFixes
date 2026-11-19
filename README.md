# Manhunt 1 Reload Fixes

An ASI plugin for Manhunt (PC, Steam) that fixes four vanilla reload bugs. Each fix can be turned
on or off in `ReloadFixes.ini`.

| Option | Bug |
|---|---|
| `RevolverFlick` | After a partial `Shotgun_(CT)` or `Shotgun_Torch_(CT)` reload, the 6-shooter's reload skips its chamber-flick. The cylinder stays open until you fire again. |
| `ShotgunQueuedReload` | Pressing reload while a shotgun shot or pump is still playing can end a partial reload with an extra pump that ejects a phantom shell. |
| `SniperLastCasing` | The sniper rifle never ejects the case from its last shot. |
| `ShotgunLastShell` | The pump shotgun's last spent shell comes out late in the reload, just before the closing pump, instead of on the pump after the last shot. |

## Install

This repo is source only, with no prebuilt binary. Build it first (see Build below).

1. You need an ASI loader in the game folder (for example Ultimate ASI Loader). If other `.asi`
   plugins already work for you, you have one.
2. Copy the built `bin\ReloadFixes.asi` and `bin\ReloadFixes.ini` into the game's `scripts\`
   folder.
3. Start the game. To uninstall, delete the `.asi`. Logging is off by default. To check that the
   fixes applied, set `Log=1` in the `.ini`: `reloadfixes_asi.log` then appears in the game
   folder with one `patched` line per enabled fix. Problems are always logged: `ABORT:` means the
   exe wasn't the expected version and that fix wasn't applied.

If you used the older `RevolverFlickFix.asi`, delete it. This plugin replaces it.

It patches the game in memory only. `manhunt.exe` is never modified. Tested on the Steam version
(image base 0x400000, files dated 2022).

## Settings (`ReloadFixes.ini`)

```ini
[Fixes]
RevolverFlick=1
ShotgunQueuedReload=1
SniperLastCasing=1
ShotgunLastShell=1

[General]
Log=0
```

`1` turns an option on and `0` turns it off. Changes take effect on the next game start.
`Log=0` (the default) only writes the log when something goes wrong; `Log=1` writes it on every
start. If the `.ini` is missing, all four fixes are on and logging is off.

## The shared cause

- `WeaponTypeData.ini` `RELOAD_LOOP a, b` sets weapon-data +0xA8 = 1, +0xAC = a, +0xB0 = b
  (parser at 0x442670). Only `6shooter` (1.067, 1.267) and `shotgun` (0.0, 0.367) have it.
- `CPed+0x33C` bit `0x40` means "this looped reload started with rounds still loaded, so stop the
  anim when the loop ends". That is how a partial shotgun reload skips the pump: the pump only
  plays after a reload that started with the tube empty.
- Only the normal shotgun reload start writes the bit (0x4B1114: rounds loaded sets it, empty
  clears it, checked for `CT_SHOTGUN` 0x2C and `CT_SHOTGUN_TORCH` 0x2D).
- `SF_TorsoReload` (0x4B86E0) obeys the bit for **any** RELOAD_LOOP weapon when it leaves the
  loop (0x4B89F9): if it's set, the anim is cut there and the torso returns to idle.
- The bit isn't cleared on weapon switch, so every path that starts a reload without writing it
  inherits a stale value.

The logic is hard-coded in the exe. The animation data isn't involved: the `6Shooter_*`,
`Shotgun_*` and `SawnOff_*` animations are identical across all the level `allanims.ifp` files.

## RevolverFlick

Repro: reload the revolver (the flick plays). Fire and reload the shotgun while it still has
shells loaded. Switch back, fire, and reload the revolver. The reload ends with the cylinder
open. Firing snaps it shut, because the fire anim resets the pose. The sawn-off doesn't cause it,
and reloading the shotgun from empty makes the revolver work again.

Cause: other guns start their reload at 0x4B1130, and the empty-clip reload starts at 0x45F479,
without touching the bit. After a partial shotgun reload it's still set, so the revolver's reload
is cut at its loop end (1.267 s). In `6Shooter_Reload_W` the cylinder swings out at 0.28–0.63 s
and only closes at 1.90–1.96 s, so it's left open.

Fix: at both sites, the 11 bytes `mov ecx,[ebp+2FCh]; call 0x425410` jump to a stub that clears
the bit (`and dword [ebp+33Ch], 0FFFFFFBFh`), runs the original two instructions, and jumps
back.

The 6-shooter is the only victim in vanilla data because it's the only other RELOAD_LOOP weapon.
A mod that gives another weapon `RELOAD_LOOP` would be affected the same way.

## ShotgunQueuedReload

Repro: empty the shotgun and reload it fully. Fire one shot and press reload repeatedly while the
pump is playing. The one-shell reload ends with a pump, and a second spent shell flies out even
though that shot's shell was already ejected. Pressing reload after the pump has finished doesn't
do this.

Cause: a reload pressed during a shot or pump is queued (0x4B10F1 sets `CPed+0x33C` bit 0x80) and
started when the pump ends, by `TorsoReloadCallback` (0x4BA0D0) at 0x4BA11E. That start never
writes bit 0x40, so the queued reload uses the value left by the previous reload. After a reload
from empty, that value says "pump at the end". The phantom shell comes from the same pump: every
reload re-arms the shell eject, so the pump ejects one whether or not a shell is in the chamber.

Fix: the 7 bytes `mov ecx,esi; call 0x425410` at 0x4BA11C jump to a stub that runs them, then sets
or clears the bit from the rounds loaded in the held weapon, exactly as the normal start does,
and jumps back.

Not a bug: after the last shot the shotgun doesn't eject a shell until you reload. A pump gun
ejects on the pump, and the game ejects the last shell at the end of a reload from empty
(`SHELL_EJECT_DELAY_RELOAD 0.7` in `entityTypeData.ini`, just past the reload loop's end, so a
partial reload never reaches it).

## SniperLastCasing

Repro: fire the sniper rifle until the magazine is empty, then reload. Every shot but the last
ejects a case when the bolt cycles, and the last case never comes out.

Cause: the case is ejected partway through the bolt cycle (`Sniper_Cock`,
`SHELL_EJECT_DELAY_FIRE 0.2` in `entityTypeData.ini`, eject at 0x4B88DD). After the last shot the
bolt-cycle state still plays, but on every frame it first sees the empty magazine and calls
`SetReload` (0x4B885D) instead of reaching the eject point. Unlike the pump shotgun, which ejects
its last shell during a reload from empty (`SHELL_EJECT_DELAY_RELOAD 0.7`), `Sniper_Rifle_(CT)`
has no reload eject.

Fix: the plugin hooks that call (0x4B8857, the 11 bytes `mov ecx,[esi+2FCh]; call 0x425410`). For
the sniper rifle (`CT_SNIPER_RIFLE`, 0x30, which the silenced sniper shares) it skips that
`SetReload` until the bolt is closed again (0.55 s; `Sniper_Cock_T` closes it at 0.491 s) and
continues into the normal bolt-cycle code (0x4B8880). That code ejects the case 0.2 s into the
bolt cycle, exactly like after any other shot. After that, the plugin requests the reload as
vanilla does but keeps running the bolt-cycle code, so the rifle's own bolt animation
(`Sniper_Cock_W`) stays synced to Cash's hands while the reload blends in.

Trade-off: the reload after an empty magazine starts about 0.5 s later than in vanilla, because
Cash finishes working the bolt first.

Earlier attempts, for the record:
- Calling the eject function at the start of the reload spawned the case (you could hear it land),
  but it flew off out of view because the rifle was still in its firing pose.
- Releasing the reload right after the eject (0.2 s) blended into the reload from a mid-bolt pose
  with the rifle's animation no longer synced, and the rifle dropped out of Cash's hands for a
  moment.

Adding `SHELL_EJECT_DELAY_RELOAD` to the sniper's record in `entityTypeData.ini` isn't enough on
its own: the sniper has no reload loop, so it would eject a case on every reload, including
partial ones.

The tranq rifle isn't affected: its "shell" (`Tranq_dart`, `SHELLS_FIRED`) is the projectile.

## ShotgunLastShell

Repro: fire the pump shotgun (`Shotgun_(CT)` or `Shotgun_Torch_(CT)`) until it's empty. Cash
pumps after the last shot like after every other shot, but no shell comes out. It only drops
late in the reload, right before the closing pump.

Cause: the same skipped eject as the sniper. The pump after a shot ejects the shell at
`SHELL_EJECT_DELAY_FIRE 0.169` (0x4B88DD), but after the last shot the pump code first sees the
empty tube and requests the reload (0x4B885D), returning before the eject point. Vanilla works
around it in data: the shotgun is the only gun with `SHELL_EJECT_DELAY_RELOAD` (0.7), timed
just past the reload loop's end so only a reload from empty reaches it. That puts the shell on
the wrong pump. On a real pump gun, the pump after the last shot ejects the spent shell, and the
pump after loading only chambers a fresh one.

Fix, in two parts:
- The same deferral as `SniperLastCasing`, at the same hook (0x4B8857), for `CT_SHOTGUN` (0x2C)
  and `CT_SHOTGUN_TORCH` (0x2D). The reload request is held until the pump has closed (0.35 s;
  `Shotgun_Cock_T` pushes it forward at 0.313 s), so the pump after the last shot ejects the
  shell. The shotgun stays synced to Cash's hands during the hand-off to the reload.
- While the reload anim is before `SHELL_EJECT_DELAY_RELOAD`, vanilla clears the weapon's "shell
  already ejected" bit on every frame (0x4B8BD7), so the reload always ejects one. For the pump
  shotguns the plugin keeps the bit. If the pump already ejected the last shell, the reload
  doesn't eject a second one. If it didn't (for example you switched weapons mid-pump), the bit
  is still clear, because firing clears it (0x4C9CB3), and the reload ejects the shell as in
  vanilla.

Trade-off: the reload after an empty tube starts a little later than in vanilla (up to about
0.35 s), because the pump finishes first.

## Build

`build.cmd` builds `bin\ReloadFixes.asi` with Visual Studio's `cl` (Win32) and copies
`ReloadFixes.ini` next to it. Nothing else is needed. Edit the `VCVARS` line in the script if
your Visual Studio is in a different place.

## Manhunt 2

Manhunt 2 has the same four bugs at different addresses. The Manhunt 2 version, with the same options, is in its own repo:
[Manhunt2ReloadFixes](https://github.com/ZT-1/Manhunt2ReloadFixes).

## Confirmed in game

- `RevolverFlick`: the revolver flick plays on every reload following a `Shotgun_(CT)` and a
  `Shotgun_Torch_(CT)` reload.
- `ShotgunQueuedReload`: with the option on, mashing reload during the pump no longer adds a pump
  or a phantom shell to a partial reload. With it off, the bug comes back.
- `ShotgunLastShell`: the pump after the last shot ejects the shell, then a full reload of 8
  ends with the closing pump and no second shell. That matches a real pump gun: racking after
  the last shot ejects the empty, and racking after loading the tube only chambers a fresh shell.
- `SniperLastCasing`: the last case is ejected and lands visibly (with its sound) after the bolt
  cycle, and the rifle stays in Cash's hands through the hand-off to the reload.
