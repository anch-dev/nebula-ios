# Nebula on iPhone and iPad

The iOS port is a work in progress (see [APPLE_PORT.md](APPLE_PORT.md)). This page is how to
build and install it today. You need a Windows PC, your own copy of Super Mario Galaxy (USA,
RMGE01, revision 0) and SideStore or AltStore on the device. No Mac is needed.

Nothing from the game is uploaded anywhere: GitHub builds the app without any game code, and
your PC compiles the game code from your own copy and adds it to the app.

## 1. Download the builds

From this repository's **Actions** tab, open the latest successful **Apple port** run and
download two artifacts:

- `Nebula-ipa`: the app (`Nebula.ipa`), without game code;
- `nebula-ios-pc-kit`: `nebula-recomp.exe` (the recompiler), `build-modules.ps1` and the iOS
  module SDK.

Unzip both into one folder, for example `C:\nebula-ios`.

## 2. Install LLVM

Install LLVM for Windows (19 or newer) from <https://github.com/llvm/llvm-project/releases>
(`LLVM-<version>-win64.exe`). The default location, `C:\Program Files\LLVM`, is where the
script looks; otherwise pass `-Llvm <folder with clang++.exe>`.

## 3. Build the game modules and the IPA

In PowerShell:

```powershell
cd C:\nebula-ios
powershell -ExecutionPolicy Bypass -File .\build-modules.ps1 `
    -Game D:\Games\SMG.rvz -Sdk .\nebula-ios-module-sdk -Ipa .\Nebula.ipa -Out C:\nebula-ios\out
```

The script checks the game, extracts it, packages its content, recompiles the game code to C++
and compiles that for iOS (this takes a while: the game is several hundred source files). The
result is `out\Nebula-SMG.ipa`, and the game content is in `out\content`.

Add `-BundleContent` to put the content inside the IPA instead (a much larger IPA, but no
copying in step 5).

## 4. Install

Install `Nebula-SMG.ipa` with SideStore or AltStore. The app asks the system for more memory
and a larger address space (`increased-memory-limit` and `extended-virtual-addressing`); these
work only when the signing account allows them.

## 5. Copy the content

Open Nebula once, so its folder appears in the Files app. Copy the `out\content` folder to
**On My iPhone › Nebula** (for example with iTunes/Apple Devices file sharing, or through
iCloud Drive and the Files app), keeping its name `content`. Then open Nebula again.

Saves are in **Nebula › saves**, the settings in **Nebula › config.ini**, and each run writes
**Nebula › nebula.log** (the previous one is `nebula.previous.log`). Please attach the log to
bug reports.

## Controls

- Touch: the lower left of the screen is the Nunchuk stick (it appears where you put your
  thumb), A jumps, ★ shoots Star Bits (Wii B), Spin shakes the remote, Z crouches, C resets
  the camera, and the arrows are the D-pad. Touch the picture anywhere else to point. The top
  row has −, Home, + , 1, 2, the pointer recenter and ⚙ (Nebula's settings overlay).
- Controllers (Xbox, PlayStation, Switch Pro and other MFi controllers) work as the Xbox
  controller does on Windows ([KEYBOARD_CONTROLS.md](KEYBOARD_CONTROLS.md)); the touch
  controls hide while one is connected.
- A hardware keyboard works as on Windows.
