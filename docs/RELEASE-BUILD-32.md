# Build 32 — multiplayer startup regression test

Serv reported that Build 31 closes when entering multiplayer. Its log confirms that the cinematic client loads and initializes its demo/camera/CineBot modules, then stops after `Waiting for renderer to initialize...`; it contains no crash stack. The previous Build 30 log progressed past renderer initialization on the same tester's machine.

Build 31 kept `unlockall` but omitted its two supporting dvars from the cinematic-client configuration. It also omitted two engine/GSC compatibility dvars. Build 32 restores those four registrations while keeping the Host, Servers, Players/kick, developer, gameplay-assist, and legacy bot-lobby controls disabled. `unlockall` and CineBot remain available. This fixes a concrete startup inconsistency; the log alone does not prove it was the only cause of the crash.

**Test:** close WWII, replace both `S2MP-Launcher.exe` and `s2mp-mod.dll` from this ZIP, then enter multiplayer. Check that a fresh `main/s2mp_console.log` says `[s2mp] package build 32`. If it still crashes, send the full fresh log and any new `s2_mp64_ship*.dmp` file; note whether it closes before or after the multiplayer menu appears. If it opens, check `unlockall`, the cinematic menu, and one short demo/dolly pass. In-game behavior of Build 32 is unverified until this test returns.
