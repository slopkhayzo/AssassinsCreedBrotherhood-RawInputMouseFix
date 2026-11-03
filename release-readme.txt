Assassin's Creed Brotherhood Raw Input Mouse Fix v@VERSION@
==========================================================

Removes the mouse acceleration of the cameras and makes the turn per mouse count
the same at every frame rate. Mouse counts come from Raw Input.

Supported game build
--------------------
Assassin's Creed Brotherhood for Windows, Ubisoft Connect version (2022-10-27
rebuild), ACBSP.exe SHA-256:
C0F6074082C99FAB4AA47715591966C727F569F32DBD3B0ACC34E43278ED157A

The hash identifies the reference build; it is not a hard allowlist. Before
installing a hook, the plugin checks the x86 PE image, its section layout and
the bytes of every hook site. A different ACBSP.exe is accepted only if all of
that still matches. A failed check skips that hook, leaves the game unchanged,
and says why in acb-raw-mouse-fix.log.

Choose an installation
----------------------
Plugin-only archive:
  Requires a compatible 32-bit ASI loader already installed. Copy
  ACBRawMouseFix.asi and acb-raw-mouse-fix.ini beside ACBSP.exe, or into the
  plugin folder of your loader. Keep both files together.

Convenience archive with Ultimate ASI Loader:
  If the game has no ASI loader, copy dinput8.dll, ACBRawMouseFix.asi and
  acb-raw-mouse-fix.ini beside ACBSP.exe. The bundled dinput8.dll is
  Ultimate ASI Loader v9.7.4, x86 build.

  If the game already has a compatible ASI loader, do not overwrite it. Copy
  only ACBRawMouseFix.asi and acb-raw-mouse-fix.ini.

For either installation:
1. Close the game before copying files.
2. Start the game normally, through Ubisoft Connect.

What it fixes
-------------
- The third-person camera (on foot, climbing, horseback) passed the mouse
  through a gamepad response curve that squares the stick, with the counts of
  one frame as the stick: faster movement turned more per count. It now turns
  by the counts themselves.
- The turn per count changed with the frame rate (at 300 FPS a movement turned
  about a fifth of what it turned at 60 FPS). It no longer depends on frame time.
- The game dropped everything above 150 counts per frame horizontally and 100
  vertically, which cut fast flicks short at low frame rates. Not anymore.
- First-person cameras get the same sensitivity settings and all counts.

Gamepads, the keyboard camera keys, menus and the map, and the game's own
sensitivity sliders and invert options keep working as before.

Usage
-----
F8   switches between the fix and the stock camera, for a comparison.
F9   lowers Sensitivity by 5 % and saves it in acb-raw-mouse-fix.ini.
F10  raises Sensitivity by 5 % and saves it.

Configuration
-------------
Edit acb-raw-mouse-fix.ini while the game is closed.

Sensitivity=1.0     global multiplier for every mouse camera, on top of the
                    game's sliders. 1.0 matches the stock camera at 60 FPS for
                    slow, precise movement.
SensitivityX=1.0    horizontal multiplier, on top of Sensitivity.
SensitivityY=1.0    vertical multiplier, on top of Sensitivity.
LinearCamera=1      the fix (0 = stock camera).
MouseScale=25       base calibration; use Sensitivity instead.
UseRawInput=1       counts from Raw Input; 0 = from the game's DirectInput polls.
RemoveClamp=1       first-person cameras get every count of a poll.
StatsLog=0          1 = write mouse and camera statistics to the log once a
                    second, for a bug report.

Diagnostics
-----------
The plugin writes acb-raw-mouse-fix.log beside the ASI. Check it if the fix
does not seem active: host, layout and signature checks and every installed or
skipped hook are listed there.

Uninstallation
--------------
Close the game and remove ACBRawMouseFix.asi and acb-raw-mouse-fix.ini. The
optional acb-raw-mouse-fix.log diagnostic file may also be removed.

Do not remove dinput8.dll if it existed before this fix or if another plugin
depends on it. Remove the bundled loader only when you installed that exact
file for this fix and no other ASI plugin needs it.

Third-party components
----------------------
The plugin includes MinHook by Tsuda Kageyu (BSD 2-Clause License). The
explicitly named convenience archive also includes Ultimate ASI Loader v9.7.4
by ThirteenAG under the MIT License. See THIRD-PARTY-NOTICES.txt.

Misc
----
For more game fixes, visit:
https://slop-blog.enkhayzomachines.net/fixes :)
