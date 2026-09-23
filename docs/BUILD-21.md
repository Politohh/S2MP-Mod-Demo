# Build 21: final free-camera roll and slower movement

Serv's Build 20 loaded engine export shows RVA 0x912EF0 clearing roll at
cg+0x23F1D4 immediately before calling AnglesToAxis at RVA 0x912F16.
That function runs after CG_CalcViewValues in CG_DrawActiveFrame, overwriting
the earlier camera roll writes. Return address: RVA 0x912F1B; callee: 0x75EE10.
Export SHA-256: FE3092934F350A1C6EEEB148834C74FF4A284B7D6A3D70E792F24FE585EB8653.

The new hook substitutes a padded copy of the angles only for that exact
caller during active native free-camera playback outside a seek. Installation
checks the callee prologue and the caller's relative call target. All other
calls pass through unchanged. The original input stays untouched. This is
a candidate world-roll correction, not a confirmed visual result. It applies
the existing manual roll setting; independent keyed roll interpolation is
not introduced by this change. demo_roll_probe reports hook state and activity.

The free-camera slider minimum is 0.1 instead of 10, with logarithmic control
and two decimal places. The maximum, reset value and playback timescale are
unchanged. Ctrl-click permits exact input within the slider limits.

Exposure blending was enabled throughout the successful supplied Build 20
test. No diagnostic exposure toggle was applied. One successful test does not
prove the intermittent darkening is resolved, so the test control remains.
Rewind/reload behavior is unchanged.

Validation: Release x64 compilation passed (existing Windows SDK macro
redefinition warnings); actual callback extraction tests passed for signed
and zero roll, preservation of source angles, exactly one original dispatch,
and seven bypass conditions. Existing Build 14 regression checks passed.
World rendering, path playback and slow movement require Serv's game test.
