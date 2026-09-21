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
//  12  Cbuf_AddText reports drops; the deferred restart retries instead of vanishing
namespace mod_build
{
	constexpr int NUMBER = 12;
}
