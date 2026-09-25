#pragma once

// =============================================================================
//  PACKAGE BUILD NUMBER — bump this every time a zip goes out
// =============================================================================
//
// WHY THIS EXISTS. Testing happens on someone else's machine: a package is
// shared, they play, and a console log comes back. Twice now a log has arrived
// with no way to tell which DLL produced it -- once the installed copy turned
// out to be a week-old build, which quietly invalidates every conclusion drawn
// from it.
//
// So the number is printed into the log at boot, right after the game build
// line, and the shared zip is named after it. A log then states its own
// provenance and "which build is this?" stops being a question anyone has to
// remember the answer to.
//
// ⛔ BUMP THIS BEFORE PACKAGING, not after. The number in the log has to be the
// number on the zip, or it is worse than having none at all.
//
//   1  first share                     demo-w2dr as cloned (efcd80e)
//   2  slow motion, roll attempt, keybind L, ProRes capture   (bdd3453)
//   3  slow motion floor pinned at 0.05, user-measured        (4921a08)
//   4  probes: does the jump move the file / does roll land   (4fbb140)
//   5  build number in the log and on the zip
//   6  skip-back: prefer keyframes the engine has a baseline for (REFUTED)
//   7  skip-back via CL_Demo_JumpToStart_f (replay runs, clock does not follow)
//   8  skip-back: seed snap.serverTime BEFORE the jump -- the rewind WORKS
//   9  stopped forcing the clock; repeat pass (regressed: force still ran on a failed pass)
//  10  never force inside the loop -- the ~1.5s rewind budget is a hard engine limit
//  11  restart+fast-forward (MW3 route) -- stranded at the menu: dropped Cbuf command
//  12  Cbuf_AddText reports drops; deferred restart retries (retries failed too)
//  13  restart-seek OFF by default (it broke scrubbing); roll via the view axis
//  23  in-place engine reset for rewind; menu-reload fallback OFF after build-22 crashes
//  24  10-bit DXGI back-buffer capture to ProRes, without changing the dolly
//  25  integrated CineBot controls, Recording tab, optional ReShade post-effect capture
//  26  4444 opaque capture, F5 recording, ADS+Use spawn, experimental F4 bot run
//  27  F4 native bot usercmd override for straight sprint to snapshot target
namespace mod_build
{
	constexpr int NUMBER = 27;
}
