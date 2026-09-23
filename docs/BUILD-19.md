# Build 19: exposure A/B test; build-18 rewind retained

Serv confirms build 18 reaches the first-camera time. Log 13 shows direct
rewinds reach 34336 ms when the cached replay lands before it. Longer
rewinds can stop at 34900 ms instead; reload #4 then completes at 34336 ms.
It also confirms clc is NULL after disconnect (connection=0), validating
the missing-data shutdown case addressed in build 18.

Serv confirms dark-to-bright happens with BOTH direct and reload seeks.
The log does not identify its renderer cause. Exposure adaptation is a
hypothesis, not a diagnosis. The map uses slow playback (0.05x).

PROBE ONLY: Demos -> Exposure test -> Disable exposure blending (test)
controls mapped r_tonemapBlend (491), whose existing mapping describes
blending between exposures. Reads plain/secure bool correctly, disables
unsupported controls, logs requested and observed state. Starts with the
engine's existing setting; no automatic override. Unchecking enables
blending again. Does not change gamma, exposure level or LightSet overrides.
Seek logs also report actual r_tonemapBlend, r_tonemapAuto and
r_tonemapUseTweaks so reload resetting a value is visible.

No rewind algorithm changes: menu reload remains the fallback for targets
not reached by cached restoration. Removing it would reintroduce failure.
The targeted engine probe adds runtime code around restore replay 0x9188C0,
existing reset/replay 0x919CE0, and policy gate 0x7DFD0 (runtime RVAs), to
investigate a true in-place restore without speculative engine writes.
World camera roll remains unresolved.

Validation: Release x64 build and existing Python/lifecycle regressions.
No live game here; this is a diagnostic package, not a confirmed fix.

Test the same short path with the checkbox unchecked then checked at the
same speed. Report whether darkening occurs on each and whether the box
stays checked after a reload. Return main/s2mp_console.log. Also run the
existing registered demo_engine_probe command in the same demo and return
main/s2mp-build-19-engine-probe.txt for the seamless rewind investigation.
