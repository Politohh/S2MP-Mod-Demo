# Build 30 — export stutter diagnostic test

Serv reports that the live 0.05× dolly preview is smooth, while its 60 FPS ProRes 4444 export has small camera jolts. Build 28's second 2560×1440 capture logged 789 written frames and three drops. The attached MP4 was compressed and edited after capture, so it cannot establish the exact timing of frames in the original MOV.

The current capture path feeds every presented frame to FFmpeg as raw video at the selected constant output rate. It does not preserve each frame's real presentation timestamp or lock the game to one output frame per demo step. If frame delivery varies, an export can have uneven movement even though the playback preview appears smooth.

Build 30 logs callback rate, short/long frame intervals, maximum readback/copy time, queue waits, and encoder drops when recording stops. It also includes an **experimental** fractional clock for the native dolly at 0.11× speed or below. That clock is enabled by default and can be turned off with `dolly_slow_clock 0` (back on with `dolly_slow_clock 1`). It only changes the camera path timing; it does not lock recording frames, change world timing, or modify seeking.

**Test:** close WWII and replace **both** `S2MP-Launcher.exe` and `s2mp-mod.dll`. Confirm `[s2mp] package build 30` in a fresh `main/s2mp_console.log`. Replay the same dolly at 0.05× and record a short 60 FPS ProRes 4444 MOV. Note whether the live view is still smooth and whether the export jolts. If it does, run `dolly_slow_clock 0` and record the same dolly once more. Send both original MOVs if practical, plus the fresh `main/s2mp_console.log`; include the MOV filename for each attempt. A compressed edit may change frame motion, so the original MOV is much more useful for diagnosis.

The source build and offline simulated-clock/spline tests passed. Export smoothness, gameplay behavior, and bot F4 behavior remain **unverified in-game** for Build 30. Bone Cam remains experimental.
