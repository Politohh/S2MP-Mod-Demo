# What we need from a CoD4 demo, and where it goes in MWR

User's requirement list (2026-08-17), mapped to concrete data. Anything not
evidenced is marked. CoD4 field names/indices are from the dumped 141-entry
playerState and 60-entry entityState tables; MWR names are from the PS4 naming
oracle and the recon dossier.

## ⭐ THE STRUCTURAL POINT: these are THREE pipelines, not one

The list looks homogeneous but splits three ways, and they are built and debugged
separately:

| pipeline | carries | source in a .dm_1 |
|---|---|---|
| **A. playerState** | local player: stance, sprint, ADS, angles, viewbob, weapon, death | `MSG_ReadDeltaPlayerstate` per snapshot |
| **B. entityState** | every other client: position, angles, anims, weapon, events | `CL_ParsePacketEntities` per snapshot |
| **C. server commands** | killfeed, score popups, hitmarkers, obituaries | `svc_serverCommand` (svc 2/3), NOT netfields |

**C is not in the netfield tables at all.** Any plan that only maps netfields
silently loses the killfeed and score popups. That is the single most important
consequence of this list.

---

# A. LOCAL PLAYER (playerState)

### 1. Sprinting vs walking

    CoD4 ps: pm_flags(16, 21 bits)         PMF_SPRINTING
             sprintState.lastSprintStart(35, -97)
             sprintState.lastSprintEnd(36, -97)
             sprintState.sprintStartMaxLength(39, 14)
             sprintState.sprintButtonUpRequired(91, 1)
             sprintState.sprintDelay(94, 1)
             legsAnim(11) / torsoAnim(28)  reflect the sprint pose

⚠ **pm_flags bits SHIFT between engines.** Bits 0-11 are identical
(PRONE=1, DUCKED=2 ... FROZEN=0x800); MWR dropped `NO_PRONE` from bit12, so CoD4
bits 13-19 shift DOWN one — **SPRINTING 0x8000 -> 0x4000**, JUMPING 0x4000 ->
0x2000, LADDER_FALL 0x2000 -> 0x1000. Conversion:
`(f & 0xFFF) | ((f & 0xFE000) >> 1) | (f & 0x1000 ? 0x2000000 : 0)`.
This is a REMAP, never a copy.

MWR sink: `pm_flags` + `sprintState.*` (MWR has the same sprintState family).

### 2. ADS vs hip fire

    CoD4 ps: fWeaponPosFrac(32, -88)   0.0 = hip, 1.0 = fully ADS  <- THE signal
             weaponstate(18, 5)        ADS states
             aimSpreadScale(14, -88)
             adsDelayTime(140, 32)
             spreadOverride(125,6) / spreadOverrideState(126,2)

MWR sink: `weapCommon.fWeaponPosFrac` (dossier confirms encoder -88 BOTH sides ->
plain value copy), `weapState[0].weaponState`, `weapCommon.aimSpreadScale`,
`weapCommon.adsDelayTime`, `weapCommon.spreadOverride*`.

⚠ `weaponstate` -> `weapState[0].weaponState` differs only in CASE (s->S). An
exact-match mapper drops it silently. And the weaponstate ENUM diverges: MWR
inserted DROPPING_ALT@5, FIRING_BALL_PASS@7 and 4 offhand states, so CoD4
FIRING 5->6, RECHAMBER 6->8, RELOADING 7->9, SPRINT 22/23/24 -> 26/27/28.
Remap table required.

### 3. Death

    CoD4 ps: pm_type(78, 8)          PM_DEAD
             events[0..3](20-23) + eventParms[0..3](24-27)   death event
             damageEvent(63)/damageYaw(65)/damagePitch(68)/damageCount(88)

MWR sink: `pm_type`, `pe.events[0..3]`, `pe.eventSequence`, damage* (same names).
⚠ Dossier: MWR's `pe.events[]` are **size 8, stride 8, encoder -94**, whereas CoD4
has separate 8-bit `events[]` + `eventParms[]`. The dossier's own note says this
"suggests MWR fused them into a {event,parm} struct" and explicitly flags it as
**HYPOTHESIS ONLY — not verified**. Settle by decompiling MWR's -94 handler
before writing any event.

### 4. Prone / crouch / standing

    CoD4 ps: pm_flags(16)             PMF_PRONE=1, PMF_DUCKED=2  (bits 0-11 SAFE)
             viewHeightCurrent(31,-88), viewHeightTarget(34,-8)
             viewHeightLerpTarget(64), viewHeightLerpTime(75), viewHeightLerpDown(71)

MWR sink: same names. Encoder -88/-8/16 identical -> value copy. **These are the
low pm_flags bits, which do NOT shift** — the only part of pm_flags that copies
directly.

### 5. Movement (local)

    CoD4 ps: origin[0..2](4,5,12, -88)      velocity[0..2](7,8,19, -88)
             speed(56,16), gravity(58,16), groundEntityNum(41,10)
             jumpTime(82), jumpOriginZ(99)

MWR sink: identical names, **encoder -88 on both sides -> straight value copy**
(dossier §2: "no conversion layer").

### 6. View angles

    CoD4 ps: viewangles[0..2](2,1,3, -87)   delta_angles[0..2](55,46,124, -100)

MWR sink: same names. `MSG_ReadAngle16`'s scale is **byte-identical** in both
binaries (0.0054931640625 = 360/65536) — proven by reading the constant in each.
Straight copy, same units.

⚠ Do NOT confuse the two. An earlier analysis mislabeled `ps+0x12C` as
`delta_angles`; it is `viewangles`. And the pinned-view bug
was `ps+0x4C` (commandTime) going stale — any synthesized ps must be
self-consistent with its own serverTime.

### 7. Viewbob

    CoD4 ps: bobCycle(6, 8 bits)
             fTorsoPitch(79), fWaistPitch(84), leanf(139)

⛔ The recon dossier's "hard field: bobCycle widens 8->16" is **REFUTED in its own
adversarial review**: the PS4 named array says `bobCycle` = offset 0xe, size 1,
**bits 8 — identical to CoD4**. PC idx15 `{0x74,-4,16}` is a *different* field.
So bobCycle is a **plain copy**, not a derivation. Do not re-derive it.

### 8. Weapon + camo (local)

    CoD4 ps: weapon(37, 7 bits)      index into that server's CS_WEAPONS
             weapons[0..3](43,44,119,96, 32)   held-weapon bitmask
             weaponold[0..3], weaponrechamber[0..3]
             viewmodelIndex(66, 9), offHandIndex(47, 7), weapFlags(40, 9)

⚠ **7 bits cannot be copied.** MWR `weapCommon.weapon` is a PACKED id: measured
live `0x00040050` = base 80 (MP5) with variant/camo in the high word. Route must
be: **CoD4 weapon index -> CoD4 configstring weapon NAME -> MWR weapon name ->
MWR packed id**. Camo rides in the same packed id's high word.

⚠ And it is coupled to the gamestate: `CG_SelectWeapon` early-bails unless the id
is precached, so weapon mapping and configstring construction are ONE problem.

⚠ CoD4's single `weapon` does NOT populate MWR's separate `weapCommon.offHand`,
`lethalWeapon`, `tacticalWeapon` — MWR may need those non-zero to assemble the
viewmodel.

---

# B. OTHER CLIENTS (entityState)

### 9. Movement + view angles (remote)

    CoD4 es: lerp.pos.trBase[0..2](2,3,4, -92/-91/-90)   position
             lerp.pos.trDelta[0..2](21,22,23)            velocity
             lerp.pos.trTime(28,-97), trType(29,8), trDuration(38,32)
             lerp.apos.trBase[0..2](19,18,33, -100)      angles
             lerp.apos.trDelta/trType/trTime/trDuration

MWR sink: the entityState list (72 fields on PS4). Encoders -92/-91/-90 (origin
X/Y/Z) and -100 (angle) exist on both sides.

### 10. Remote stance / anims / weapon

    CoD4 es: legsAnim(47,10), torsoAnim(46,10), fWaistPitch(41), fTorsoPitch(42)
             weapon(7, 7 bits), weaponModel(8, 4)
             eType(0,8), lerp.eFlags(1,-98), clientNum(20, 7 bits)
             partBits[0..3](55-58, 32)

⛔ **anims are the hard one.** `legsAnim`/`torsoAnim` widen 10->12 bits, but width
is the trivial part — they are **indices into a per-model/per-map anim table**, so
a CoD4 index means something else in MWR even after widening. There is no numeric
mapping. Either resolve via anim NAME through both anim tables, or accept wrong
anims in v1 and drive the body from origin/velocity/movementDir.
**Do not copy the integer.**

⚠ `clientNum` is **7 bits in CoD4 (0..127)** and **6 bits in MWR (0..63)**. 64
players fit; 65+ do not. This is the wire-level statement of the 64-vs-18 issue —
see `00_differential_map.md` §1: the ARRAYS are already relocated to 64 by
`cgame_slots.cpp`, and the wire supports 64.

---

# C. SERVER COMMANDS — killfeed, score popups, hitmarkers

**None of this is in the netfield tables.** It arrives as `svc_serverCommand`.

### 11. Killfeed at exact server time
### 12. Score popup messages
### 13. Hitmarkers

    CoD4 : svc_serverCommand carries the obituary / score / feedback text.
           The demo stores these in the message stream alongside snapshots, so
           the SERVER TIME is the enclosing snapshot's serverTime -> "exact
           server time" is satisfied for free IF commands are kept in order.

    MWR  : svc 2 (TEXT, long seq + string<=1024)
           svc 3 (BINARY, long seq + short len)   <- MWR-era binary commands
           consumed by CG_DeployServerCommandString / CG_ServerCommand,
           rendered by LUI (LUI_Obituary for the killfeed).

⚠ **MWR's killfeed and score popups are LUI-driven, CoD4's are not.** MWR renders
obituaries through LUI (memory `mwr-custom-killfeed` records that h1-mod had to
draw its OWN CoD4-style killfeed because the LUI clock was unreachable). So a
CoD4 obituary string cannot be replayed verbatim — it must either be translated
into MWR's own server-command vocabulary, or rendered by our own killfeed
component (which already exists: `src/client/component/demo/killfeed.cpp`).

⚠ Reliable commands must be **executed**, not force-acked — h1-mod's own comment
records that force-acking silently discarded `setConfigstring` for custom weapon
variants, which is why public-match demos never resolved them.

### 14. Gun sounds

    CoD4 : ps events[0..3] + eventParms[0..3]  (local)
           es events[0..3](5,24,25,32, -94) + eventParms[0..3](9,26,27,37, -93)
           es loopSound(48, 8)

MWR sink: `pe.events[0..3]` (local), entityState events (remote), `loopSound`.
Blocked on the same unresolved question as death events: whether MWR fused
`{event, parm}` into one 8-byte record. **Settle the -94 handler first** — events
drive sounds, deaths, hitmarkers and impacts, so this one unknown gates a large
part of the list.

---

# CROSS-CUTTING RULES (from the dossier's design section, adversarially reviewed)

1. **Bits/encoder from the PC table; NAMES from the PS4 table.** `pm_flags` is 31
   bits on PS4 and 32 on PC — encoding with PS4 widths desyncs the bitstream. The
   PS4 IDB is a NAMING ORACLE ONLY.
2. **Join on NAME, never on index.** CoD4 idx1 = `viewangles[1]`; MWR idx1 =
   `pe.eventSequence`. The orders are unrelated.
3. **Exact string match is insufficient** — ~21 aliases, some differing only by
   case (`weaponstate`->`weapState[0].weaponState`, `throwBackGrenade*`->
   `throwbackGrenade*`). Encode them literally, with a startup assertion that
   every CoD4 field is classified exactly once as {mapped | dropped} so a missed
   rename is loud.
4. **MWR-only fields get a deliberate default, never a template's leftover value.**
   That exact class of bug caused the pinned view (stale `ps+0x4C`).
   MWR-only: vehicleState.*, perkSlots[0..8], weapState[1].* (akimbo),
   unpredictableEvents[], link*, radar*, turn*, dofPhysical*, shieldState.flags.

# STILL TO ADD when the user thinks of more

Candidates already visible in the CoD4 table that are not on the list yet but are
cheap once the pipeline exists: `perks`(51), `radarEnabled`(77),
`iCompassPlayerInfo`(118) (minimap), `shellshockTime/Duration/Index`(89,90,92),
`holdBreathScale/Timer`(29,80), `meleeChargeTime/Yaw/Dist`(115,116,117),
`mantleState.*`, `killCamEntity`(52), `cursorHint*`(72,73,74),
`iHeadIcon/iHeadIconTeam` (es 43,44).

---

# ⛔ CORRECTIONS (same session, found while mapping)

## 1. CoD4 has PER-ENTITY-TYPE field tables — remote players use a DIFFERENT one

`re/cod4/iw3_entitystate_netfields.json` (60 fields, `clientNum`@idx20,
`legsAnim`@47) is the **GENERIC** `entityStateFields[59]`. Other players do NOT
use it. CoD4's full set (KisakCOD, cited in the dossier):

    playerStateFields[141]              the local player
    playerEntityStateFields[59]         ET_PLAYER  <- OTHER PLAYERS
    entityStateFields[59]               generic ET_GENERAL/5/10/11/15/16
    vehicleEntityStateFields[59]        planeStateFields[60]
    helicopterEntityStateFields[58]     clientStateFields[24]
    hudElemFields[40]

`playerEntityStateFields[59]` layout (verified @0x6B7200, sv_msg_write_mp.cpp:81):

     0 eType            1 pos.trBase[0]   2 pos.trBase[1]   3 lerp.u.player.movementDir
     4 apos.trBase[1] YAW                 5 pos.trBase[2]   6 eventSequence
     7 apos.trBase[0] PITCH               8 legsAnim        9 torsoAnim   10 lerp.eFlags
    11..14 events[0..3]  15..18 eventParms[1,0,2,3]  19 groundEntityNum
    20 fTorsoPitch  21 fWaistPitch  22 solid  23 weapon  24 eventParm
    25 pos.trType   26 apos.trType  27 apos.trBase[2] ROLL   28 clientNum(7 bits)

So requirement 9/10 (remote movement, angles, anims) map through THIS table.
**My first pass pointed them at the generic table's indices — wrong.**

⭐ This mirrors MWR exactly: `MSG_GetStateFieldListForEntityType` clamps to 25
types. Both engines dispatch the entity field list BY ENTITY TYPE, so the
transcode must too — pick the source table by CoD4 eType, pick the sink table by
MWR eType, and map within the pair. Never one flat entity mapping.

## 2. ⭐ PROVEN — MWR FUSED {event, parm} in playerState; CoD4 did not

    CoD4 ps : events[0..3]      8 bits each, offsets 0xB8..
              eventParms[0..3]  8 bits each, offsets 0xC8/CC/D0/D4  (stride 4)
              -> SEPARATE arrays

    MWR  ps : pe.eventSequence  off 344, size -4, enc -75
              pe.events[0..3]   off 348/356/364/372, size 8, enc -94
              -> stride 8, size 8, and there is NO eventParms[] array

This resolves the dossier's flagged "HYPOTHESIS ONLY". The offsets prove it:
348->356 is +8, and no separate parm array exists. **MWR packs {event, parm} into
one 8-byte record per slot.**

⚠ But entityState is the OPPOSITE: BOTH engines keep `events[]`(-94) and
`eventParms[]`(-93) separate there. So the fusion is playerState-only. The
transcoder needs two different event strategies depending on which table it is
writing.

This unblocks requirements 3 (death), 13 (hitmarkers) and 14 (gun sounds), which
all ride the event channel.

## 3. `clientStateFields[24]` is the roster pipeline

CoD4 `clientStateFields[24]` -> MWR `ClientState` (56 fields). This is the
packet-clients section that `CG_SetNextSnap` reads to build `clientinfo` — i.e.
**player NAMES, team, and model index**. The earlier "no gun + no body"
investigation bottomed out here: ci is rebuilt each snapshot from the snapshot's
client state, not from configstrings.

Requirement-wise this is what makes other players *exist* at all, so it ranks
alongside A and B rather than being an afterthought. 24 -> 56 fields means most
MWR client fields have no CoD4 source and must be defaulted deliberately.

---

# ⭐⭐ CORRECTION 4 — THE KILLFEED IS AN ENTITY EVENT IN **BOTH** ENGINES

My "pipeline C" split put killfeed/score/hitmarkers on the server-command
channel. **For the killfeed that is WRONG**, and the correction makes the job
much easier.

## PROVEN — CoD4 side

`GScr_Obituary` @0x12e9a0 (CoD4, fully named):

    String   = Scr_GetString(2);                      // weapon NAME
    weapIdx  = G_GetWeaponIndexForName(String);
    mod      = G_MeansOfDeathFromScriptParam(3);      // means of death
    victim   = Scr_GetEntity(0);
    te       = G_TempEntity(vec3_origin, 66);         // <- TEMP ENTITY, event 66
    te[116]  = victim entnum
    te[120]  = attacker entnum   (1022 when there is none)
    te[246]  = 8

So the CoD4 killfeed is a **temp entity carrying event 66**, with payload
{victim, attacker, weapon, means-of-death}. It travels in the ENTITY stream.

## PROVEN — MWR side

    LUI_Obituary @0x1857e0:
        LUI_BeginEvent(localClient, "obituary", L)
        hksi_lua_setfield(L, -2, "attacker")  ... named Lua fields
    callers of LUI_Obituary: **exactly one — CG_EntityEvent @0x2a9c50**

`CG_EntityEvent` (source `D:\h1\code_source\Runtime\cgame\cg_event.cpp`)
references `'(event > 0)'`, `'(event < EV_MAX_EVENTS)'`,
`'ent:%3i event:%3i params:%3i '`, **`'killicondied'`, `'killiconmelee'`**, and
`'weaponIdx < (MAX_WEAPONS + MOD_NUM)'`.

⭐ **Both engines transport the killfeed as an ENTITY EVENT.** Only the RENDERING
differs (CoD4 native vs MWR LUI). And `MAX_WEAPONS + MOD_NUM` confirms MWR packs
means-of-death into the same index space as the weapon — the same shape as CoD4's
obituary payload.

### Consequences

1. Requirement 5 (**killfeed at exact server time**) moves from pipeline C to
   pipeline B. "Exact server time" then comes **for free**: the event rides its
   snapshot, so it inherits that snapshot's serverTime. No separate timeline.
2. The conversion is **event-number + payload mapping**, not text translation:
   CoD4 event 66 -> MWR's obituary EV_ constant, and {victim, attacker,
   weapon, MOD} -> MWR's equivalent params.
3. We do NOT have to defeat LUI. Feeding MWR the right entity event makes MWR's
   own LUI killfeed render it — which is what "looks natural, normal" requires.
   (h1-mod's `killfeed.cpp` custom renderer stays a FALLBACK, not the plan.)

## Revised pipeline model

| pipeline | carries | note |
|---|---|---|
| A. playerState | stance, sprint, ADS, angles, viewbob, weapon, death-state | fused `pe.events` |
| B. entityState + EVENTS | remote movement/angles/anims, **killfeed**, gun sounds, impacts, hitmarkers(?) | the big one |
| C. server commands | scores, configstring updates, text | smaller than first thought |

`CG_EntityEvent` is the single most important function on the MWR side — it is
where killfeed, sounds and effects all land. Mapping CoD4's event enum onto
MWR's EV_ enum is now the highest-value remaining unknown for this list.

## Score popups (requirement 4) — MWR uses a SPLASH TABLE

`CG_DeployServerCommandString` references **`mp/splashTable.csv`**. MWR's score
popups are the "splash" system (medal/points popups) driven from that table.
S2 uses the equivalent system (`SplashesWidget`,
`ui_player_splash_id_<n>` / `ui_player_splash_param_<n>` omnvars), and MWR is the
same family one generation earlier.

⚠ So score popups are NOT free-text: a CoD4 score event must be mapped to an MWR
**splash id** from `mp/splashTable.csv`. That is a content mapping table we do not
have yet, and it is a genuine CoD4->MWR divergence (CoD4 has no splashTable).

## Still open on this list

  * **hitmarkers** — not yet located in either engine. Candidates: an entity
    event via CG_EntityEvent, or client-side damage feedback from `damageEvent`/
    `damageCount` in playerState. NOT established either way; do not assume.
  * **CoD4 event enum -> MWR EV_ enum** — the mapping that gates killfeed, gun
    sounds, deaths and impacts. Both enums are recoverable (CoD4 is fully named;
    MWR's EV_ names are in the PS4 build).

---

# ⭐⭐⭐ THE EVENT ENUM MAPPING — RECOVERED (2026-08-17)

Both engines keep a contiguous `EV_*` name table in address order, so position ==
enum value. Extracted with `re/find_ev_enum.py`, joined by name in
`re/map_events.py` -> `re/event_map.json`.

    CoD4  135 events, run @0x35bdc6, starts at EV_NONE (index 0)
    MWR   185 events, run @0xe8f4e8, starts at EV_FOLIAGE_SOUND

⚠ MWR's table omits index 0. The base offset was **verified, not assumed**: base 1
gives 4 name/value coincidences vs 0 for base 0. MWR enum value = position + 1.

    mapped by name                 65
    CoD4-only                      70   (but see LANDING below - really ~12)
    MWR-only                      120
    mapped AND same numeric value   4   (6%)

⭐ **Only 6% keep their value, so events MUST be renumbered.** Copying the event
integer is guaranteed wrong - this is the event-channel analogue of the netfield
"join on NAME, never on index" rule.

## ⭐ CROSS-VALIDATION — EV_OBITUARY

`GScr_Obituary` calls `G_TempEntity(vec3_origin, **66**)`. Independently, the enum
table says `cod4[66] = **EV_OBITUARY**`. Two unrelated sources agree, which
confirms both the obituary chain AND that position == enum value.

    EV_OBITUARY   cod4 66  ->  mwr 141

## Requirement -> event mapping (all name-matched)

    KILLFEED (req 5)
      EV_OBITUARY                 66 -> 141

    GRENADES (new req)
      EV_PREP_OFFHAND             32 -> 59    pull pin
      EV_USE_OFFHAND              33 -> 60    THE THROW
      EV_SWITCH_OFFHAND           34 -> 62    frag <-> flash/smoke
      EV_GRENADE_BOUNCE           44 -> 94
      EV_GRENADE_EXPLODE          45 -> 98
      EV_FLASHBANG_EXPLODE        48 -> 103
    ps fields: offHandIndex(47), offhandSecondary(76), grenadeTimeLeft(85),
               throwBackGrenadeOwner(53), throwBackGrenadeTimeLeft(103)

    WEAPON SWITCH primary<->secondary (new req)
      EV_PUTAWAY_WEAPON           23 -> 33    holstering
      EV_RAISE_WEAPON             21 -> 31    bringing up
      EV_FIRST_RAISE_WEAPON       22 -> 32    initial spawn raise
      EV_WEAPON_ALT               24 -> 34    alt-mode toggle
      EV_PULLBACK_WEAPON          25 -> 37
      EV_RECHAMBER_WEAPON         28 -> 42
    ps fields: weapon(37) changing + weaponstate(18) DROPPING/RAISING +
               weapAnim(17) + weaponTime(13) + weaponold[0..3]

    GUN SOUNDS / FIRING (req 14)
      EV_FIRE_WEAPON              26 -> 39
      EV_FIRE_WEAPON_LASTSHOT     27 -> 40
      EV_STOP_WEAPON_SOUND         2 ->  2   (one of the 4 same-value events)
      EV_RELOAD..EV_RELOAD_ADDAMMO 15..20 -> 25..30  (contiguous, +10)

    HITMARKERS (req 13) - candidates, NOT yet proven
      EV_BULLET_HIT               41 -> 77
      EV_BULLET_HIT_CLIENT_SMALL  42 -> 80
      EV_BULLET_HIT_CLIENT_LARGE  43 -> 81
      EV_MELEE_HIT                35 -> 68
    The *_CLIENT_* variants are the likely hitmarker trigger (a hit ON a client),
    but that is INFERENCE from the name - confirm against CG_EntityEvent's handler
    before relying on it.

    STANCE (req 8)
      EV_STANCE_FORCE_STAND/CROUCH/PRONE   6,7,8 -> 11,12,13

    FOOTSTEPS / MOVEMENT FEEL
      EV_FOOTSTEP_PRONE  75 -> 156   EV_FOOTSTEP_WALK 74 -> 159
      EV_FOOTSTEP_RUN    73 -> 160   EV_FOOTSTEP_SPRINT 72 -> 161
      EV_JUMP            76 -> 162

## ⭐ The 70 "CoD4-only" events are mostly ONE structural difference

    58 of 70 are EV_LANDING_<SURFACE>

CoD4 encodes the **surface** in the landing event (EV_LANDING_ASPHALT, _BARK,
_BRICK, _CARPET, _CLOTH, _CONCRETE, _DIRT, _FLESH, ... plus _PAIN_ variants).
MWR has only **four**, by **intensity**:

    163 EV_LANDING_LIGHT   164 EV_LANDING_MEDIUM
    165 EV_LANDING_HEAVY   166 EV_LANDING_PAIN

So this is a redesign, not a gap: **many->few**, mapped by fall intensity, with
the surface carried separately (CoD4 entityState has `surfType` at idx 10).
Collapsing these leaves only ~12 genuinely unmapped CoD4 events.

## Renames and splits needing a hand alias (same pattern as the netfields)

    CoD4 EV_MELEE_SWIPE / EV_FIRE_MELEE  ->  MWR EV_FIRE_MELEE_SWIPE(56) /
                                             EV_FIRE_MELEE_STAB_START(57)
    CoD4 EV_EMPTY_OFFHAND (single)       ->  MWR EV_EMPTY_OFFHAND_PRIMARY(20)
                                             or EV_EMPTY_OFFHAND_SECONDARY(21)
        ^ a 1->2 SPLIT. Choose using ps `offhandSecondary`(76).

MWR-only additions with no CoD4 source (leave unsent): EV_USE_OFFHAND_THROWBACK(61),
EV_WEAPON_SWITCH_STARTED_OFFHAND(36), EV_OFFHAND_END_NOTIFY(23), EV_EMP_OFFHAND(22),
the EV_SOUND_ALIAS_* pitch/volume family.

## ⚠ There is NO EV_DEATH / EV_DIE in either engine

Death (req 3) is conveyed by `pm_type` (PM_DEAD) plus `EV_OBITUARY`, not a
dedicated event. Do not go looking for a death event.

---

# READING A REAL .dm_1 — MEASURED STATE (2026-08-17)

## ⭐ TWO DIFFERENT .dm_1 CONTAINER FORMATS EXIST IN THE WILD

Sampled 14 demos across the user's CoD4 installs:

    Type A  first byte 0x00   6/14   the format the dossier verified
    Type B  first byte 0x02   8/14   02 <len=19|20> ff ff ff ff 00*8 <4 bytes>

The dossier's spec (KisakCOD, verified) is:

    block := u8 type
      0 : i32 serverMessageSequence ; i32 msglen ; u8 payload[msglen]
      1 : i32 index ; u8 archive[48]                  (53 bytes on disk)
      other : reader IGNORES the byte and consumes nothing

So marker 2 is **not a CoD4 1.7 record**. Type B leads with `ffffffff` (the Q3
connectionless/OOB marker). Both types appear in the SAME directory, so it is not
a per-install difference. **Identify Type B before promising "any CoD4 demo"** -
more than half the sampled corpus is Type B, and the reader consumes 0 bytes of it
(`unknown marker 2 @ 0`).

## ⭐ TYPE-A DECODE IS FAR BETTER THAN THE OLD INVENTORY CLAIMED

`PORT_INVENTORY.md` records "snap decode ~2% full snaps on vacant". Measured now
on the user's real demos:

    con1.dm_1      mp_convoy/war      81 pkts    617 archives   79/80   = 98.8%
    demo0000.dm_1  mp_crash /war   2,461 pkts  7,985 archives 2057/2491 = 82.6%

    decompression: 2461 ok / 0 fail
    svc census   : gamestate 1, snapshot 1721, serverCommand 739
    map resolved : mp_crash -> mp_crash (a REAL MWR map), gametype war
    weapons seen : c4, claymore, saw, m60e4, m4  (from configstrings)

So the CoD4 read side is in much better shape than the record suggested. The 2%
figure evidently came from a different (probably Type-B or vacant) demo.

## ⚠ WHAT IS STILL NOT SURFACED — the actual gap for "readable"

Despite 82-99% snapshot decode, the reader's OUTPUT is missing most of the
requirement data:

    players  : 1        <- only the local slot; remote players NOT extracted
    entities : []       <- EMPTY on every frame
    events   : 0        <- EMPTY, so killfeed/sounds/grenades are not surfaced
    frame0 pose: origin/angles/velocity all 0.0

The timeline is still sourced from CLIENT ARCHIVES (POV only), which is why
`players` is 1 and entities are empty. The snapshots ARE being decoded - the data
exists - it is simply not being lifted into the timeline.

**This is the concrete next task for "we need all this readable":** wire the
decoded snapshot's playerState + packet-entities + packet-clients into the
timeline output, instead of falling back to archives. Everything needed to
interpret those fields (both netfield tables, the event enum map, the entity-type
table split) is now mapped.

## Requirement readiness

| requirement | field mapping | readable from a real demo TODAY |
|---|---|---|
| sprint/walk | done (pm_flags + sprintState) | no - ps not surfaced |
| ADS/hip | done (fWeaponPosFrac) | no - ps not surfaced |
| stance | done (pm_flags low bits) | no |
| movement (self) | done (origin/velocity) | archives only (POV) |
| view angles | done (viewangles -87) | archives only |
| viewbob | done (bobCycle, 8 bits both) | archives only (disk+32) |
| weapon + camo | route known, needs name->packed id | weapon NAMES already readable |
| weapon switch | done (EV_PUTAWAY/RAISE + ps.weapon) | no - events not surfaced |
| grenades | done (EV_PREP/USE/SWITCH_OFFHAND) | no - events not surfaced |
| death | done (pm_type + EV_OBITUARY) | no |
| killfeed | done (EV_OBITUARY 66 -> 141) | no - events not surfaced |
| gun sounds | done (EV_FIRE_WEAPON 26 -> 39) | no - events not surfaced |
| movement (others) | done (playerEntityStateFields) | no - entities empty |
| score popups | NOT done - needs splashTable mapping | no |
| hitmarkers | candidate events only, unproven | no |

---

# ⭐⭐ THE XP POPUP ("+10") — ANSWERED. IT IS A HUD ELEMENT, NOT A SPLASH.

User clarified: "score popup" means the per-kill XP popup (`+10`), which CoD4
renders using the localized string **`MP_PLUS`**.

## PROVEN

    CoD4  "MP_PLUS" @0x395efe   — a LOCALIZED STRING KEY, no direct code xref
                                  (looked up by name; driven from GSC _rank.gsc)
    MWR   "MP_PLUS"             — DOES NOT EXIST anywhere in the binary

    CoD4  splash functions      — NONE (its only Sys_*SplashWindow are the
                                  loading-screen window, unrelated)
    MWR   splash system         — PlayerCmd_ShowHudSplash @0x5641c0 (GSC builtin),
                                  CG_CloseSplashes @0x2bea20, GetSplashMethodOfDeath
                                  @0x591460, mp/splashTable.csv

So the two engines do NOT share a mechanism, and my earlier "map CoD4 score events
onto MWR splash ids" was the wrong route: **CoD4 has no splash system to map from.**

## ⭐ THE ACTUAL CHANNEL — hudelems ride inside the PLAYERSTATE

CoD4's playerstate wire order (msg_mp.cpp:1584-1776), step 10:

    if (ReadBit()) { MSG_ReadDeltaHudElems(hud.archival, 31);
                     MSG_ReadDeltaHudElems(hud.current,  31); }

    MSG_ReadDeltaHudElems (msg_mp.cpp:1530-1582):
        inuse = ReadBits(5)
        for i < inuse { lc = ReadBits(6)
                        for j in 0..lc INCLUSIVE: ReadDeltaField(&hudElemFields[j]) }
        ^ note <= lc: an OFF-BY-ONE versus every other field loop. Get this wrong
          and the whole playerstate desyncs after the hudelem section.

    hudElemFields[40]  — server_mp.h:30-72, uses the -85/-86/-99 codecs

MWR has the SAME channel:

    MSG_ReadDeltaHudElems @0x728f40 / MSG_WriteDeltaHudElems @0x741d90
    assert: count == MAX_HUDELEMS_ARCHIVAL || count == MAX_HUDELEMS
    HudElem netfield list @0x12D7CB0 — 44 fields (CoD4 has 40 / KisakCOD 43)

⭐ **So the `+10` popup transports as a hudelem in BOTH engines, in the same place
in the snapshot, through the same two-array (archival + current) structure.**
The conversion is **hudelem -> hudelem**, 40 -> 44 fields joined by name — the
same pattern as the playerstate. No splashTable mapping needed.

## What still has to be handled

  1. **The string reference.** A hudelem carries a localized-string / configstring
     INDEX, not literal text. CoD4's index resolves to `MP_PLUS`; MWR has no such
     key, so the index must be remapped to an MWR string that renders "+N" (or the
     text supplied directly if the hudelem supports it). This is a CONTENT mapping,
     small but real.
  2. **Does MWR still RENDER classic hudelems?** The plumbing is unambiguous - the
     netfield table, both readers/writers, and the MAX_HUDELEMS asserts all exist.
     But MWR's HUD is LUI-driven, and this project has already proven (S2) that LUI
     can own things the native path still carries. **NOT ESTABLISHED** that a fed
     hudelem draws on screen. Verify before relying on it; the fallback is MWR's
     own splash system via the mechanism `PlayerCmd_ShowHudSplash` uses.

## Bonus finding from the same wire-order section — WHY POSES READ AS ZERO

Step 5 of the playerstate order:

    lc = ReadBit()                      // "sendOriginAndVel"
    ...
    if (!lc) CL_GetPredictedOriginForServerTime(cl, to->commandTime,
                 to->origin, to->velocity, to->viewangles,
                 &to->bobCycle, &to->movementDir)   <- from cl->clientArchive[256]

**When that leading bit is 0, origin/velocity/viewangles/bobCycle/movementDir are
NOT on the wire at all** — the client pulls them from its own archive ring. That
is exactly why the decoded frames showed origin/angles/velocity as 0.0, and it
explains why the archives carry the POV motion.

⇒ The reader MUST fuse snapshot + client-archive to reconstruct local motion.
Those are the 8 `changeHints == 3` "predicted" fields: origin[0..2],
velocity[0..2], bobCycle, movementDir.

## And the entity two-stage dispatch is confirmed on the CoD4 side too

    lc = MSG_ReadLastChangedField(msg, 61) = ReadBits(6)
    ReadDeltaField(&entityStateFields[0])                 // eType from the GENERIC table
    fieldList = MSG_GetStateFieldListForEntityType(to->eType)   // then pick the real table
    for i in 1..lc-1: ReadDeltaField(&fieldList->array[i])

eType is field 0 of EVERY table at the same offset(4)/bits(8) - that is what makes
the two-stage read possible. **MWR does exactly the same.** Both sides of the
transcode must dispatch by entity type.

---

# ⭐ UNIFORMS / FACTIONS (new requirement) — the clientState pipeline

User: uniform depends on FACTION (spetsnaz / marines / opfor) AND on weapon class
(sniper vs shotgun changes the model).

In CoD4 both of those are baked into the MODEL NAME, e.g.
`body_usmc_desert_assault_mp`, `body_usmc_desert_sniper_mp`,
`body_opforce_woodland_assault_mp`. The client never computes it - the server
sends a model INDEX and the client resolves it through configstrings.

## PROVEN — both clientState layouts

    CoD4 clientState_s (client_mp.h:176-196, sizeof 0x64), 24 netfields:
      clientIndex 0x00 | team 0x04 | modelindex 0x08
      attachModelIndex[6] 0x0C | attachTagIndex[6] 0x24
      name[16] 0x3C                      <-- INLINE 16-char STRING
      maxSprintTimeMultiplier 0x4C | rank 0x50 | prestige 0x54 | perks 0x58
      attachedVehEntNum 0x5C | attachedVehSlotIndex 0x60

    MWR ClientState, 56 netfields (recovered, all named):
      [12] modelindex           off=2    10 bits   <-- THE UNIFORM
      [11] team                 off=104   2 bits
      [29] name                 off=152  10 bits   <-- an INDEX, not text
      [8..43]  attachModelIndex[0..19]   10 bits each
      [17..53] attachTagIndex[0..15]      5 bits each
      [23] customization.numModels        [54] variants[0] 32  [55] variants[1] 8
      [24..26] perks[0..2]      [13] dualWielding
      [27] nameplateFriendlyIndex  [28] nameplateEnemyIndex
      [0..6] compressedAnimData.*

## The uniform conversion route

    CoD4 modelindex -> CL_GetConfigString(modelindex + 830)   = CS_MODELS base
         -> "body_usmc_desert_assault_mp"
         -> [NAME->NAME table]  ->  MWR model name
         -> MWR modelindex, resolved against CL_GetConfigString(idx + 1240)

⚠ The two config bases differ: **CoD4 CS_MODELS = 830, MWR model base = 1240**.
The index is meaningless across engines - only
the NAME transfers, exactly like weapons and anims.

⚠ Faction+class is therefore a **content mapping table**, not logic: enumerate the
CoD4 body_* names a demo references and pair each with an MWR equivalent. Since
CoD4 encodes class in the name, sniper/shotgun variants fall out automatically -
no weapon inspection needed at transcode time.

## ⛔ A REAL DIVERGENCE — player NAMES are carried differently

    CoD4  clientState.name = INLINE char[16]      (the text travels on the wire)
    MWR   ClientState.name = a 10-bit INDEX       (resolves via a string table)

So a CoD4 player name cannot be copied into MWR's clientState - an index must be
ALLOCATED for each name and the corresponding string installed, or names come out
blank/wrong. This is the same coupling as weapons: the roster and the
gamestate/configstring construction are one problem.

⚠ MWR-only, needing deliberate defaults: customization.numModels, variants[0..1],
dualWielding, nameplateFriendly/EnemyIndex, compressedAnimData.*, and
attachModelIndex[6..19] / attachTagIndex[6..15] (CoD4 only has 6 of each).
CoD4-only, dropped: rank, prestige, maxSprintTimeMultiplier, attachedVeh*.

---

# ⭐ HUDELEM TABLE RECOVERED — confirms the "+10" XP popup mechanism

MWR HudElem, 44 netfields (all named). The fields that matter for the popup:

    [20] text        off=132  10 bits   <-- STRING INDEX (CoD4's MP_PLUS equivalent)
    [21] value       off=128  float     <-- THE NUMBER (10, 50, 100...)
    [36] label       off=64   10 bits
    [13] x  [2] y  [14] z                 -85/-91/-92 position codecs
    [1]  color.rgba  [17] fromColor.rgba  [23] glowColor.rgba   (-85)
    [12] fontScale   [25] fromFontScale   [18] font  [3] type  [19] sort
    [15] fadeStartTime [16] fadeTime      [22] time  [43] duration
    [37] moveStartTime [38] moveTime      [32] scaleStartTime [33] scaleTime
    [5]  targetEntNum  [7] materialIndex  [8] width [9] height

So a "+10" is a hudelem with `text` = a string index and `value` = 10, plus
position/colour/fade - and both engines carry it in the same place in the
playerstate. The conversion is field-by-field with ONE content mapping (the string
index), exactly as concluded earlier.

⚠ `duration` [43] came back with no offset/bits from the initializer emulation -
the last record's numeric stores were not captured. Re-check that one field before
using it.
