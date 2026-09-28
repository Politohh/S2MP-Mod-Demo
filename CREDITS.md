# Credits and provenance

- Original project: [Rattpak/S2MP-Mod](https://github.com/Rattpak/S2MP-Mod).
- Demo branch used as the starting point: [josh155/S2MP-Mod, `demo-w2dr`](https://github.com/josh155/S2MP-Mod/tree/demo-w2dr), commit `efcd80e520f8c597db1b56d4b0953a63b0c58225`.
- This continuation: Polito, with testing and feedback from serv. The original Git commit history remains available in this repository.

The upstream repositories do not currently provide a repository-level license. This file acknowledges provenance; it does not grant permission to use, redistribute, or relicense code owned by others. ReShade API headers in `src/third_party/reshade` carry their own notice in `src/third_party/reshade/LICENSE.md`. Other bundled dependencies retain their respective notices in their directories.

From v1.0.2 (Build 35), the release archive includes the S2MP launcher, mod DLL, documentation, hashes, the ReShade header notice, and a standalone FFmpeg executable for ProRes recording. It does not include the Call of Duty: WWII executable or ReShade binaries.

FFmpeg 9.0.2 is built from the [official FFmpeg source](https://ffmpeg.org/download.html) under LGPL version 2.1 or later, with GPL, nonfree, and external codec libraries disabled. The client launches it as a separate process. The archive's `FFmpeg` folder includes the license, matching unmodified source, build script, build configuration, and toolchain notices. The build script is also in [tools/Build-CaptureFFmpeg.sh](tools/Build-CaptureFFmpeg.sh).
