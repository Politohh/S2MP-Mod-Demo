# Build 17: restart-seek candidate from runtime-code evidence

## Evidence
Serv confirms build 16 No HUD works. The supplied engine probe identifies
build 16, Steam PE timestamp 0x6963A4D9. Probe SHA256: ab24f928e5c2a74a5c32b34b5589451133fe250b0da6e948f16fb17526b9197b.
The subsequently supplied s2mp_console (11).log confirms build 16. Its early
three restore slots have empty replay ranges; the later slot at 309200 ms
is after the requested first-camera times (including 305301 ms). At the
last reported check, all 3440 generation-policy checks were rejected and
zero keyframes were generated through the instrumented generator. This
explains cache refusal; it does not establish which engine policy gate
rejected generation. Reload fallback avoids depending on those missing
cache entries. The earlier log (10) identifies build 15.

The probe at RVA 0x4A15C0 (existing _b literal 0x4A05C0) scans two local-client
active flags with stride 0x7B8 and returns -1 when neither qualifies (except
its special frontend fallback). It does not allocate a command buffer.
GameUtil discarded commands on -1, explaining why retries could never reopen
playback at the menu. The function's historical name was misleading.

The old restart poller also accepted the still-active old demo as the reopened
one. Both bugs are addressed, rather than retrying the old restart unchanged.

## Changes
- Use the caller's local-client index when the active-client selector returns
  -1. Validate index, command-buffer metadata and append bounds.
- Older targets with no eligible cached restore now request reload+advance.
  Cached restores that land short also use that fallback. No forced target
  timestamp is written into snapshot data.
- Wait for old session closure, then a new playback generation with an active
  cgame and native demo state. Only then queue a token-checked seek callback.
- J resumes after the seek. Go/left arrow preserve pause. Restore camera mode
  and playback speed. Suspend dolly driving during direct and reload seeks.
- Deferred reopening uses a cancellable callback, preserves the requested
  path and retries failed command insertion until its bounded timeout.
- Stop invalidates pending reopen/completion commands. Duplicate seeks cannot
  overwrite an existing restart transaction. Failed completion stays paused.
- Keep the confirmed No HUD change; no additional roll changes.

## Validation and limits
Release x64 build passed. Compiled tests extract the actual command-routing,
restart-poll/completion and deferred-play code and exercise normal operation,
old/new session distinction, delayed readiness, queue rejection, stale tokens,
Stop cancellation, changed demo, timeout, paused/running requests and failed
completion. Existing seek-policy and Python regressions passed.
These are mocked lifecycle tests, not gameplay acceptance. Reload, correct
world snapshot timing, main-menu recovery and path playback need Serv's test.
No hard engine rewind limit is asserted. World roll remains unresolved.

## Reproduction for Serv
Use build 17. Place cameras across several seconds; pause after the final one.
Press J and allow the reload to finish. Compare action timing with the first
camera's time. Test Go on first/later cameras, then left arrow with/without
camera points, once paused and once playing. Test Stop during reload once.
Send the NEW main/s2mp_console.log and confirm its build-17 banner.
