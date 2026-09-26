# Build 33 — multiplayer startup crash fix

Serv's Build 32 dump identifies an access violation in `s2mp-mod.dll!Load_Texture_hookfunc`: the image-loading hook reads through a null `g_dumpImages` dvar while multiplayer UI textures load. The cinematic client intentionally omits this developer-only dvar. Build 33 checks whether the dvar exists in both texture-loading hooks before reading it. Image replacement and the cinematic-client feature set remain available. `unlockall` and CineBot remain enabled; Host, Servers, Players/kick, and developer controls remain removed.

Build 32's restored `unlockall` and compatibility dvars are retained, but the dump shows they were not sufficient to prevent this crash. The launcher executable is unchanged from the previous working package.

**Test:** close WWII and replace both `S2MP-Launcher.exe` and `s2mp-mod.dll` from this ZIP. Enter multiplayer and check that a fresh `main/s2mp_console.log` says `[s2mp] package build 33` and progresses past `Waiting for renderer to initialize...` into the multiplayer UI. If it still closes, send the fresh log and any new `s2_mp64_ship*.dmp` file. If it opens, confirm `unlockall`, CineBot, and one short dolly playback. In-game behavior remains unverified until serv tests this build.
