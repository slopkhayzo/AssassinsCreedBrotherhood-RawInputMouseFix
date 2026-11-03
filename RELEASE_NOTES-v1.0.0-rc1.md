# Assassin's Creed Brotherhood Raw Input Mouse Fix v1.0.0-rc1

First release candidate.

## What it does

- The third-person camera (on foot, climbing, horseback) turns by raw mouse
  counts. The game passed the mouse through a gamepad response curve that
  squares the stick, with one frame's counts as the stick, so faster movement
  turned more per count, and the turn per count fell as the frame rate rose. The
  fix skips that curve on the game's mouse path only.
- The turn per count is the same at every frame rate.
- No per-frame clamp: the game dropped everything above 150 counts per frame
  horizontally and 100 vertically.
- Counts come from Raw Input, read on DirectInput's own raw-input window, with the
  game's DirectInput polls as a fallback. Every count reaches each camera once;
  counts from while the camera wasn't running are dropped.
- Settings: `Sensitivity` (global, F9/F10 in game), `SensitivityX`,
  `SensitivityY`. They apply to the third-person and first-person cameras.
- F8 switches between the fix and the stock camera.

## Tested

A 22-minute session on the Ubisoft Connect build (on foot, climbing, horseback;
1920×1080, 360 Hz, ~300 FPS): every Raw Input count reached the camera, none in
the wrong direction, and the counts matched the game's own. Built-in tests cover
the count transport from 20 to 777 FPS.

Not yet specifically tested: Leonardo's machines, cannons and other special
controls, and other frame rates in game.

## Release archives

- `ACBrotherhood-RawInputMouseFix-v1.0.0-rc1-plugin-only.zip`: the ASI, its
  configuration, documentation, license, notices and payload hashes, for
  installations that already have a compatible 32-bit ASI loader.
- `ACBrotherhood-RawInputMouseFix-v1.0.0-rc1-with-Ultimate-ASI-Loader-v9.7.4.zip`:
  the same, plus Ultimate ASI Loader v9.7.4 x86 as `dinput8.dll`.

Each archive has a `.sha256` file beside it.
