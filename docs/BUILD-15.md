# Build 15: partial update, not a rewind or world-roll fix

Log 9 requests J at 307380 ms. The only eligible cached slot is 309200 ms.
The old selector replaces the requested target with that later timestamp;
subsequent restoration lands around 309500 ms. Camera point selection itself
already uses the first point. No evidence establishes a hard engine rewind limit.

Changes: refuse a backward seek with no cached frame at/before the target;
propagate failure so J does not unpause after a failed seek; show seek failure
in the Demos panel. Left arrow and the backward UI button request -10 seconds
relative to current demo time, independently of camera points. Right remains +5.
No HUD toggles the mapped cg_draw2D dvar (2562), and reads its actual value.
Remove the FOV-hook axis write demonstrated to displace nametags rather than
roll the rendered scene. Existing roll angle plumbing remains unresolved.

Limits: ten-second rewinds and J still cannot reach a target outside the current
cache. Automatic demo restart stays disabled because earlier tests stranded the
tester at the menu. This build prevents misleading success; it does not implement
full-range rewind. The local protected executable could not be decompiled at the
known view-builder address; an unpacked S2 binary/database is needed to trace the
renderer and native snapshot restore without guessing at new hook locations.

Validation: Release x64 build; compiled production seek-policy tests covering
log 9 and range/eligibility boundaries; three offline Python regression checks.
No game was launched, and no in-game behaviour is claimed verified.
