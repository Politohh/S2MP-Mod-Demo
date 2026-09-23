# Build 16: HUD state correction and rewind investigation

## Proven from source and log 10
- Build 15 displayed cg_draw2D.current.enabled without checking the dvar type.
  The project supports DVAR_TYPE_BOOL_SECURE via decodeDvarSecureBool.
  The GUI now decodes that type and retains ordinary bool support. Requests
  log the actual type, on/off direction, and queue success.
- J requests the first point at 305387 ms. Accepted cache contains slot 3 at
  309200 ms. Targets before it are rejected by build 15's explicit guard.
- Go to 317280 ms restores snapshots around 316950-317100, then advances to
  317280. Serv reports that recent-camera seek works visually.
- The supplied log has one visible map-load sequence for mp_battleship_2;
  multiple demos are not independently demonstrated by that file.

## Candidate and unknown
Secure bool decoding explains the HUD checkbox symptom but its actual runtime
type is not in log 10. Build 16 logs it. World roll and earlier rewinds remain
unresolved. No claim that camera 4 has special handling, that the cache limit
is an engine limit, or that this is a full seek fix.

## Additional evidence collection
Failed backward seek logs up to 16 populated raw slots, replay bounds,
payload accumulation, rejection reasons, read count and keyframe-generation
policy checks/acceptances. Reports are throttled to once per two seconds.
The existing generation policy and rewind mechanism are unchanged.

New explicit command: demo_engine_probe. Copies six bounded engine code
ranges from readable executable pages inside the game module into
main/s2mp-build-16-engine-probe.txt. No gameplay buffers, user data, network
transmission or game binary modifications. Code may include active mod hooks.
The report covers view building, keyframe generation/restoration, freecam and
command-buffer selection, allowing further offline disassembly.

## Validation
Release x64 build passed. Actual GUI value-selection block tested with secure
and plain bools, missing dvar and unsupported type. Export memory guards tested
against executable, non-executable, no-access and cross-page ranges; guarded
copy tested for success and fault. Existing compiled seek-policy and three
Python regression checks passed. No game runtime test performed.
