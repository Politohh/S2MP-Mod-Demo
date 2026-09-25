# Build 29 — 0.05× dolly smoothing test

Serv's Build 28 2560×1440, 60 FPS ProRes 4444 recording reported 789 frames and 3 encoder drops. The 60 FPS compressed example shows smaller background-motion jumps roughly every 20 frames, including between camera points. At 0.05× playback, this spacing fits a 16–17 ms native demo-clock step stretched across about one third of a second.

Build 29 gives the **native dolly camera only** a fractional clock at 0.11× speed or below. It integrates elapsed time at the game's selected demo speed and re-anchors on rewind, pause, speed changes, and long stalls. It does not change demo seeking, game world timing, ProRes encoding, or normal-speed dolly playback. One log line after 120 camera updates reports how often the original engine clock repeated and its largest step; this helps verify the diagnosis on the test machine.

**Test:** close WWII and replace **both** `S2MP-Launcher.exe` and `s2mp-mod.dll`. Confirm `[s2mp] package build 29` in a fresh `main/s2mp_console.log`. Replay the same dolly at 0.05× and record a short 60 FPS ProRes 4444 MOV. Compare the live camera and export with Build 28. Send the fresh `main/s2mp_console.log` and a short clip if any jolts remain. The recording stop line still reports encoder drops separately. Keep the original MOV for frame-by-frame comparison; a compressed edit can introduce its own changes.

The source build and offline simulated-clock/spline tests passed. Visual smoothness and bot F4 behavior remain **unverified in-game** for Build 29. Bone Cam remains experimental.
