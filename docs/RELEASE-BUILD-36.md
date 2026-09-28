# S2MP Mod v1.0.3 (Build 36)

Built-in game configs now execute once through the engine. The mod previously read them again and queued duplicate match-setting commands, filling the command buffer during match setup. Local configs in `players2`, including `autoexec.cfg`, still use the mod's command handling.

This build also logs the name, type, and size of any network-asset table the engine rejects during map loading. It preserves the engine's checks and limits.

## Install and launch

Close WWII and copy all the extracted files and folders into the game folder, next to `s2_mp64_ship.exe`. Replace older copies. Run **S2MP-Launcher.exe** and click **Multiplayer**. **F9** or **Insert** opens the tools menu.

FFmpeg is included. The dolly, recording, and CineBot controls are unchanged.
