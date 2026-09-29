# S2MP Mod v1.0.6 (Build 39)

Console `/map` cleanup now runs in the engine's original server unload stage. The extra asset release inside the loading-screen helper has been removed, and private-match menu preloading uses the original engine request again.

The log now records live packet flow, command creation and server-client state to locate the remaining private-match loading stall. These diagnostics do not change connection state or bypass streaming checks.

The demo tools, CineBot, config-loading fix, launcher and bundled FFmpeg are included.

## Install and launch

Close WWII and copy all extracted files and folders into the game folder, beside `s2_mp64_ship.exe`. Replace older copies. Run **S2MP-Launcher.exe** and click **Multiplayer**. **F9** or **Insert** opens the tools menu.
