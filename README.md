# S2MP Mod v1.0.1

![S2MP demo tools open over a Call of Duty: WWII scene](docs/screenshots/demo-tools.png)

This is the Call of Duty: WWII client I've been using for demos and cinematics.

I didn't build it from scratch. [Rattpak made the original S2MP Mod](https://github.com/Rattpak/S2MP-Mod), and [Josh (josh155) built on it in his demo-w2dr branch](https://github.com/josh155/S2MP-Mod/tree/demo-w2dr). Josh started the cinematic side and did about 90% of the work on this version. He later handed it over to me and gave me the go-ahead to keep working on it and release it.

I'm Polito. I came at this as an editor: I used the client to make cines, tested it as I went, and polished the demo, dolly, camera, and recording tools around that workflow. serv tested the builds with me, caught a lot of issues, and confirmed the demo and dolly workflow is working on his setup. Thanks to both of them, and to Rattpak for the original project. The original Git history is still here; [CREDITS.md](CREDITS.md) has the full source and third-party credits.

v1.0.1 is Build 34. It keeps the Build 30-based cinematic setup and the default `dolly_slow_clock 0`. I also raised `com_maxfps` to 1000, so you can set it from the console or the FPS control.

## Install

1. Download [S2MP-Mod-v1.0.1-build-34.zip](https://github.com/Politohh/S2MP-Mod-Polito/releases/download/v1.0.1/S2MP-Mod-v1.0.1-build-34.zip) and extract it.
2. Close WWII. Put both S2MP-Launcher.exe and s2mp-mod.dll next to your s2_mp64_ship.exe, replacing any older copies.
3. Start S2MP-Launcher.exe and choose Multiplayer. Press F9 or Insert to open the tools menu.

The ZIP includes the launcher, mod DLL, instructions, and hashes. It does not include the game, FFmpeg, or ReShade.

## Making a cine

- Record a match or open a demo in the Demos tab. F1 opens the timeline.
- Go to Dolly and turn on **Drive the camera**. K places a camera point, L removes the last one, and J rewinds and plays the path.
- Left Arrow skips the demo back ten seconds. In free cam, use the mouse wheel for roll or Alt + wheel for FOV. Camera speed and demo timescale are in the menu.
- The camera tools include FOV, roll, third-person framing, and No HUD.
- The Recording tab can capture ProRes through FFmpeg. F5 starts or stops capture. ReShade effects need the compatible add-on setup in [RESHADE-SETUP.txt](docs/RESHADE-SETUP.txt).
- CineBot is there for private/custom matches. ADS + your Use key spawns a bot at the crosshair; F7 moves the selected bot and F8 toggles freeze.

This rollback also includes the Host, Servers, Players, and developer controls from Build 30.

## In game

The dolly path, with three camera points placed in a demo:

![Three camera points and the Dolly tab in a WWII demo](docs/screenshots/dolly-path.png)

The Recording tab, set up for ProRes:

![ProRes recording settings over a WWII cinematic scene](docs/screenshots/prores-recording.png)

And a frame from the finished camera work:

![Cinematic frame of a soldier in a snowy WWII scene](docs/screenshots/cinematic-frame.png)

## Build from source

On Windows, use Visual Studio 2022 with the C++ desktop workload and Windows SDK. Run tools/premake5.exe vs2022, then build s2mp-mod.sln as Release | x64. The DLL will be in bin/Release/. The launcher is a separate upstream binary.

If something goes wrong, send me the fresh main/s2mp_console.log. For bot issues, also include S2CineBot.log from the game directory.

I haven't added a new license to the inherited S2MP code. See [CREDITS.md](CREDITS.md) for the project history and notices.
