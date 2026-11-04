# Assassin's Creed Brotherhood Raw Input Mouse Fix

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested a couple hours of gameplay and so far spotted no major issues (on my machine ofc, if you have issues feel free to open an Issue)
> this patch currently probably only works for the latest Ubisoft Connect and Steam versions of the game.
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/assassin-s-creed-brotherhood-raw-input-mouse-fix) :)

A runtime patch for the 32-bit PC version of *Assassin's Creed Brotherhood*. It
removes the mouse acceleration of the cameras and makes the turn per mouse count
the same at every frame rate. Mouse counts come from Raw Input.

Current release: **v1.0.1**

## Compatibility

| Build | `ACBSP.exe` SHA-256 | Status |
| --- | --- | --- |
| Ubisoft Connect (2022-10-27 rebuild, PE timestamp `1666877377`) | `C0F6074082C99FAB4AA47715591966C727F569F32DBD3B0ACC34E43278ED157A` | Reference build; live-tested |
| Steam (the same build wrapped in SteamStub DRM 3.1) | `72FE53EB40CAE0F17D8EA457A6E12694B32A3B41D0D88B8BF79169750556C174` | Reference build; live-tested since v1.0.1 |

The Steam executable is the Ubisoft Connect build with its code encrypted by
SteamStub. Decrypted, the code is byte-identical, so both use the same hook sites.
If the ASI loader loads the plugin before the stub has decrypted the code, the
plugin waits for the decryption and installs its hooks right after it, instead
of rejecting the encrypted bytes.

The SHA-256 is an identity diagnostic, not an allowlist. Before installing any
hook, the ASI checks that the host is a PE32 x86 `ACBSP.exe` with the researched
section layout, and checks the byte signature of every hook site, with relocated
operands masked and then checked by value. A different executable is accepted
only if all of that still matches. A failed check skips its hook, says why in the
log, and leaves the game as it is. Nothing is checked again after startup.

## What it fixes

- **Mouse acceleration.** The game treats the mouse as a virtual right stick and
  passes it through a gamepad response curve that squares the stick's length. For
  a thumbstick that's an ordinary curve. For the mouse the "stick" holds the counts
  of one frame, so the turn per frame grew with counts squared: the faster you
  moved, the more each count turned. The fix skips the curve on the game's mouse
  path and turns the camera by the counts themselves.
- **Sensitivity that changed with the frame rate.** With the curve, the turn per
  count was proportional to mouse speed divided by frame rate: at 300 FPS a
  movement turned about a fifth of what it turned at 60 FPS. Nothing in the fixed
  path depends on frame time, so the turn per count is constant.
- **The per-frame clamp.** The game drops everything above 150 counts per frame
  horizontally and 100 vertically, which cut fast flicks short at low frame rates.
  Not anymore.
- **Raw counts.** Counts are read from Raw Input on DirectInput's own raw-input
  window (no second registration, so the game's mouse keeps working), with the
  game's DirectInput polls as a fallback. Every count reaches the camera exactly
  once; counts collected while the camera isn't running (menus, cutscenes) are
  dropped instead of being replayed.

The third-person camera covers walking, climbing, the horse and the other
third-person views. First-person cameras were already linear in the game; they
get the same sensitivity settings and the unclamped counts.

Unchanged: gamepads, the keyboard camera keys, menus and the map, the game's own
sensitivity sliders and invert options (they still apply), pitch limits and
camera collision.

## Current status

v1.0.0-rc1 passed a 22-minute play session on the Ubisoft Connect build (on foot,
climbing, horseback; 1920×1080 at 360 Hz, ~300 FPS): every Raw Input count
reached the camera, none turned the wrong way, and the counts matched what the
game itself saw. The stock camera's effective multiplier during that session
ranged from 3× to 192× depending on speed; with the fix it's one constant.

v1.0.1 adds the Steam build: an unattended start to the menus and a play session
through Steam, with every hook installed and the same count statistics.

Not yet specifically tested: Leonardo's machines, cannons and other special
controls, and other frame rates in game (frame-rate independence is by design and
covered by the tests below).

## Build

Install Visual Studio 2022 or newer, or the Build Tools, with the **Desktop
development with C++** workload, then run from a Command Prompt:

```bat
build-msvc.cmd
```

The script always sets up the **x86** toolchain, builds
`build\ACBRawMouseFix.asi`, and builds and runs the tests:

- `mouse-model-test`: the count transport, at 20 to 777 FPS, with camera
  gaps, blocked input, a single count arriving between poll and camera, two
  cameras blending, wrap-around and two threads.
- `watch-test`: the development watchpoint tool, including a stress test of its
  debug-register race.
- `asi-load-test`: the ASI loaded into a host that isn't the game, and into a
  copy of that host named `ACBSP.exe`; both must be refused.

To also check the plugin against a real executable, set its path first:

```bat
set "ACB_EXE=C:\path\to\Assassin's Creed Brotherhood\ACBSP.exe"
build-msvc.cmd
```

`layout-profile-test.exe` then maps the executable into its own process without
running it, runs the plugin's validation and hook installation against that
private mapping, and checks every hooked entry. The file on disk is never
modified.

For the Steam executable, also pass a copy unpacked with
[Steamless](https://github.com/atom0s/Steamless) (for this test only; never ship
or install it):

```bat
set "ACB_STEAM_EXE=C:\path\to\steamapps\common\Assassins Creed Brotherhood\ACBSP.exe"
set "ACB_STEAM_UNPACKED=C:\path\to\ACBSP.exe.unpacked.exe"
build-msvc.cmd
```

The test maps the encrypted file, checks that nothing is hooked while its code is
encrypted, writes the decrypted code into the mapping as the stub would, and
checks that the deferred installation then hooks everything.

## Package

After building, create the plugin-only archive and the convenience archive with
the tested loader:

```powershell
.\package-release.ps1 -Version 1.0.1 -LoaderPath C:\path\to\x86\dinput8.dll
```

The loader must be the x86 `dinput8.dll` of Ultimate ASI Loader v9.7.4; the
script checks its version and pinned SHA-256. It writes per-file and archive
SHA-256 files, includes the loader's notice only in the bundle, and refuses to
overwrite an existing release. Omit `-LoaderPath` for the plugin-only archive.

## Install

1. Close the game.
2. Install a compatible **32-bit** ASI loader, such as Ultimate ASI Loader, as
   `dinput8.dll` beside `ACBSP.exe`. If the game already has a compatible loader,
   keep it.
3. Copy `ACBRawMouseFix.asi` and `acb-raw-mouse-fix.ini` beside `ACBSP.exe`, or
   into the plugin folder of your loader. Keep the two together.
4. Start the game normally, through Ubisoft Connect or Steam.

The convenience archive also contains the tested Ultimate ASI Loader v9.7.4 x86
as `dinput8.dll`. Use it only if the game has no loader yet; don't overwrite an
existing one. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
is MIT-licensed; its notice is in `THIRD-PARTY-NOTICES.txt`.

The plugin writes `acb-raw-mouse-fix.log` beside the ASI: what it checked, which
hooks it installed, and why one was skipped.

## Configuration

Edit `acb-raw-mouse-fix.ini` while the game is closed.

- `Sensitivity=1.0`: global multiplier for every mouse camera, on top of the
  game's own sliders. 1.0 matches the stock camera at 60 FPS for slow, precise
  movement. **F9/F10** in game lower/raise it by 5 % and save it in the ini.
- `SensitivityX=1.0`, `SensitivityY=1.0`: horizontal and vertical multipliers on
  top of `Sensitivity`.
- `LinearCamera=1`: the fix. **F8** in game switches between the fix and the
  stock camera for a comparison.
- `MouseScale=25`: base calibration of the third-person camera. Use
  `Sensitivity` instead.
- `UseRawInput=1`: counts from Raw Input. `0` uses the game's DirectInput polls.
- `RemoveClamp=1`: cameras that read the game's own counts (first person) get
  every count of a poll instead of at most 150/100.
- `StatsLog=0`: `1` writes mouse and camera statistics to the log once a second,
  for a bug report.

## Uninstall

Close the game and remove these files from the game folder:

- `ACBRawMouseFix.asi`
- `acb-raw-mouse-fix.ini`
- `acb-raw-mouse-fix.log` (optional diagnostic log)

Don't remove a shared or pre-existing ASI loader.

## Technical outline

The patch is an x86 ASI plugin loaded by an external ASI loader. It exports
nothing and doesn't forward `DirectInput8Create`. After the startup checks it
installs these MinHook hooks:

- the third-person camera's stick curve (`0x007A1240`). On the game's own mouse
  path (keyboard and mouse selected, mouse active) it replaces the stick with
  the counts since that camera's previous update times the configured scale,
  keeping the game's sign, invert and dominant-axis rules; otherwise the
  original curve runs;
- the first-person camera's input (`0x008D83F0`), whose mouse deltas get the
  sensitivity multipliers. Values the game didn't write in an update are left
  alone;
- the keyboard and mouse to virtual pad mapping (`0x00B95D30`), for the
  unclamped counts and statistics.

Raw Input is read by subclassing the window DirectInput registered for the mouse.

On the Steam build, if the code is still SteamStub-encrypted when the plugin
loads, the hooks wait: the plugin patches three imports the game's C runtime
calls right after the stub hands over (`GetSystemTimeAsFileTime`,
`GetCommandLineA`, `GetStartupInfoA`), installs the hooks on the first call that
finds the code decrypted, and restores the imports.

The repository contains the source and the build and package scripts. Release
archives contain only the built plugin, its configuration, documentation and
hashes, and, in the explicitly named convenience archive only, the third-party
ASI loader with its notice. No game file is modified or distributed.
