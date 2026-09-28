# S2MP Mod v1.0.5 (Build 38)

Map loading now clears the previous level on the private-match menu route. Console `/map` also clears frontend and level assets after the renderer has synchronized, before the next loading screen is installed.

This replaces Build 37's cleanup at a server unload call that Multiplayer skips. Client activation is logged to help locate any remaining loading-screen stall.

The demo cleanup and config-loading fix are included. The package includes the launcher and FFmpeg.

## Install and launch

Close WWII and copy all extracted files and folders into the game folder, beside `s2_mp64_ship.exe`. Replace older copies. Run **S2MP-Launcher.exe** and click **Multiplayer**. **F9** or **Insert** opens the tools menu.
