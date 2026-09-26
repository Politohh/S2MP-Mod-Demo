# S2MP Mod v1.0 — WWII cinematic toolkit

An unofficial Call of Duty: WWII (S2) multiplayer demo and camera toolkit. This repository continues the [`demo-w2dr` branch of josh155/S2MP-Mod](https://github.com/josh155/S2MP-Mod/tree/demo-w2dr), which is itself a fork of [Rattpak/S2MP-Mod](https://github.com/Rattpak/S2MP-Mod). The original commit history is retained. See [CREDITS.md](CREDITS.md) for provenance and third-party notices.

Version **1.0** packages Build 33, a rollback to the Build 30 code with `dolly_slow_clock` off by default. The DLL prints `[s2mp] version 1.0` and `[s2mp] package build 33` in `main/s2mp_console.log` so reports can be tied to the installed binary. This is a game-build-specific mod, not a copy of WWII.

## Get started

1. Download `S2MP-Mod-v1.0-build-33.zip` from the [v1.0 release](https://github.com/Politohh/S2MP-Mod-Polito/releases/tag/v1.0). Extract `S2MP-Launcher.exe` and `s2mp-mod.dll` beside your own `s2_mp64_ship.exe`.
2. Close the game before replacing either file. Start it with `S2MP-Launcher.exe`.
3. Open the tools window with **F9** or **Insert**. **F1** toggles the timeline.
4. Record or select a multiplayer demo on the **Demos** tab. In **Dolly**, enable **Drive the camera**, place points, and press **J** to rewind and play the path.

The ZIP includes installation instructions and SHA-256 hashes. The game executable, ReShade, and FFmpeg are not redistributed here.

## What is in this build

| Area | Current state |
| --- | --- |
| Demos and dolly | Native demo playback, free camera, camera points, path playback, rewinds, FOV, and world-camera roll. The optional fractional camera clock is **off by default** (`dolly_slow_clock 0`); `dolly_slow_clock 1` enables it. |
| Recording | Native demo recording and an FFmpeg/ProRes capture tab. Capture logs source-frame cadence and capture pressure at stop to diagnose export-only stutter. ReShade effects require a compatible full add-on ReShade installation; see the package's `RESHADE-SETUP.txt`. **F5** starts or stops capture. |
| CineBot | In a private/custom match, **ADS + bound Use** spawns at the crosshair, **F7** moves the selected bot, and **F8** toggles freeze. **F4** attempts a straight run toward a snapshot of the player's position. The Build 28 F4 command interception is **awaiting in-game validation**. |
| HUD | The **No HUD** control toggles the game's `cg_draw2D` setting. |
| Bone Cam | Still experimental; do not rely on it for a shoot. |
| Host and developer controls | The Build 30 rollback includes the Host, Servers, Players, and developer controls. Use this mod for private cinematic testing. |

Camera shortcuts: **K** adds a dolly point, **L** removes the last point, **J** rewinds and plays the path, and **Left Arrow** seeks back ten seconds. Some shortcuts are ignored while the UI is focused. In free camera, the mouse wheel adjusts roll and **Alt + wheel** adjusts FOV. Camera speed can be reduced in the UI. Demo speed can reach **0.05×**; lower speeds previously caused severe stutter on the test machine.

## Build from source

The source and dependency trees are in this repository. On Windows, use Visual Studio 2022 or later with the C++ desktop workload and Windows SDK. Generate the solution with `tools/premake5.exe vs2022`, then build `s2mp-mod.sln` as **Release | x64**. The output is under `bin/Release/`. The launcher EXE is a separately supplied upstream binary; this repository builds the mod DLL.

Release builds no longer copy files into a developer-specific game directory. Use your own install path when testing. Engine hooks and CineBot offsets target the exact WWII executable described in `src/net/cinebot_constants.hpp`; other game versions are not validated.

## Feedback and provenance

The v1.0 release has passed source-build, dolly-clock, dolly-curve, and package-integrity checks. The Build 33 rollback has not yet been verified in-game by serv. Bot F4 behavior and Bone Cam remain experimental; ProRes export can still show uneven motion when capture callbacks fall below the requested output FPS. It has not been certified for online or anti-cheat use.

For a problem report, include the ZIP name, the fresh `main/s2mp_console.log`, and, for bot issues, `S2CineBot.log` from the game directory. Describe what the game visibly did; an offline build cannot prove in-game behavior. See [v1.0 release notes](docs/RELEASE-BUILD-33.md) for this package's test focus.

This repository does **not** declare a new license for the inherited S2MP code. Neither [josh155/S2MP-Mod](https://github.com/josh155/S2MP-Mod) nor [Rattpak/S2MP-Mod](https://github.com/Rattpak/S2MP-Mod) currently declares a repository license. Their respective copyrights remain with their authors. Third-party files retain their own notices.
