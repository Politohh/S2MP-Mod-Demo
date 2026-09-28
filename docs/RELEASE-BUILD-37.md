# S2MP Mod v1.0.4 (Build 37)

Private-match menu and console map launches now release the previous level's assets at the server's existing unload step, before loading the next map. This addresses image-pool exhaustion when starting a map on top of retained hub or level assets.

The demo loader's separate cleanup is restricted to demo playback. It no longer performs another teardown later in live match startup.

Build 36's config-loading fix is included. The launcher and FFmpeg are included.

## Install and launch

Close WWII and copy all extracted files and folders into the game folder, beside `s2_mp64_ship.exe`. Replace older copies. Run **S2MP-Launcher.exe** and click **Multiplayer**. **F9** or **Insert** opens the tools menu.
