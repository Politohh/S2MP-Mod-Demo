# Build 28 — private preview

Build 28 keeps the dolly/rewind, world-camera roll, No HUD, and ProRes capture work from earlier builds. Its code change intercepts the WWII test-client command consumer used by CineBots. When **F4** is pressed, each tracked bot remains frozen until its commands are actually intercepted; the intended behavior is to face the player's position at keypress time, sprint straight toward it, and avoid firing. A bot that receives no matching command within 500 ms stays frozen and logs the reason.

**Validation:** the Release x64 DLL compiled and the offline regression checks passed during packaging. The remote tester previously reported dolly and demo skipping functional and successfully recorded ProRes with the compatible ReShade add-on. The Build 28 CineBot F4 behavior has **not** yet been confirmed in-game. Bone Cam remains experimental.

To test F4, install both binaries from `S2MP-Mod-build-28-private-release.zip`, confirm `[s2mp] package build 28` in a fresh `main/s2mp_console.log`, then spawn two or three bots in a private/custom match with **ADS + bound Use**. Press **F4** once. They should run toward the position where you pressed it, without shooting or redirecting when you move. Kill a bot to check that it returns frozen to its saved spawn point. Send the fresh `main/s2mp_console.log` and game-directory `S2CineBot.log` if it fails.

The ZIP includes `S2MP-Launcher.exe`, `s2mp-mod.dll`, setup notes, third-party notice, and SHA-256 hashes. No game binary or optional capture dependency is bundled.
