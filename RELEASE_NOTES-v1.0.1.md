# Assassin's Creed Brotherhood Raw Input Mouse Fix v1.0.1

## What changed

- **Steam version supported.** The Steam `ACBSP.exe` is the same build as the
  Ubisoft Connect one, wrapped in SteamStub DRM: its code is encrypted on disk and
  decrypted when the game starts. Decrypted, it's byte-identical, so the fix uses
  the same hook sites. If the ASI loader loads the plugin before SteamStub has
  decrypted the code, the plugin used to find encrypted bytes, reject every hook
  site and leave the game stock. It now waits for the decryption and installs the
  hooks right after it. The log names the Steam build.
- The log keeps a section per game process instead of each process overwriting
  the previous one. A launch through Steam or Ubisoft Connect can start
  `ACBSP.exe` more than once.

There are no new settings. An existing `acb-raw-mouse-fix.ini` from v1.0.0-rc1
can be kept.

## Tested

On the Steam build: an unattended start through Steam to the menus, and a play
session through Steam. Every hook was installed, the counts the fix applied
matched the game's own, and none turned the wrong way. Offline, the plugin's own
checks run against the encrypted Steam executable and against its code decrypted
in memory. On the Ubisoft Connect build, everything from v1.0.0-rc1 still applies.

## Release archives

- `ACBrotherhood-RawInputMouseFix-v1.0.1-plugin-only.zip`: the ASI, its
  configuration, documentation, license, notices and payload hashes, for
  installations that already have a compatible 32-bit ASI loader.
- `ACBrotherhood-RawInputMouseFix-v1.0.1-with-Ultimate-ASI-Loader-v9.7.4.zip`:
  the same, plus Ultimate ASI Loader v9.7.4 x86 as `dinput8.dll`.

Each archive has a `.sha256` file beside it.
