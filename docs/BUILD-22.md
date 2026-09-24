# Build 22: repeated dolly replay and reload stability candidates

Serv confirmed that Build 21 rolls the rendered camera and showed no
exposure darkening in this test. The supplied console logs both identify
Build 21. In log 15, repeated J presses use keyframe slot 4 at 27,900 ms,
replaying a 102-message span each time before the log ends. There is no
crash dump or exception address, so the exact crash site remains unknown.

The dolly marker renderer previously checked cgame state but did not check
whether a native seek was in progress. The seek reparses gamestate on the
client thread while marker rendering can use its view on another thread;
an earlier project dump established this class of crash. Build 22 gives
marker rendering and synchronous native seeks a shared mutex. Rendering
tries the mutex and skips that frame if a seek holds it. Bonecam also avoids
resolving an entity during a native seek. These changes target a concrete
race; they are not a claim that the new crash has the same stack.

In log 16, the left-arrow target had no usable earlier keyframe. The
existing fallback reopened the demo, reached the requested time, and
restored pause immediately. Build 22 allows three Presents after the new
session becomes ready before applying the seek, then allows three more
rendered frames before restoring pause. The final timestamp can therefore
advance slightly beyond the target when the arrow seek was originally
paused. This is an explicit tradeoff for avoiding a black loading frame.
Arrow presses now report whether they were gated or queued, so forward
skip failures can be separated from input issues in the next log.

Camera position and angle interpolation now scales its tangents by the
real time between points. This keeps velocity continuous at a marker when
adjacent camera segments have unequal lengths. Keyed FOV is unchanged.

The 0.05 speed floor remains because serv measured severe FPS loss below it.
Bonecam functionality is not claimed fixed; only its seek safety was changed.

Validation: Release x64 compiled; actual dolly interpolation extraction
test passed continuity and two-point cases; seek policy and restart-flow
tests passed, including paused warmup and session teardown. None of these
offline checks substitute for repeated in-game J and arrow-key tests.
