# CoD4 → MWR differential map

**Goal:** play a stock CoD4 PC `.dm_1` demo in MWR/h1-mod natively. No simulation,
no puppeting — MWR's own `CL_ParseSnapshot` / `CG_*` consume the data and it looks
normal.

Evidence is labeled by confidence. Nothing here is inferred
from the other engine unless it says so.

## Reference builds

| port | binary | role |
|---|---|---|
| 19337 | `iw3mp_dump.exe` (CoD4X) | CoD4 PC target, PE @0x400000, 8,987 funcs / 16.7% named |
| 18337 | `Call of Duty 4 Multiplayer` | **CoD4 reference, Mach-O, 9,140 funcs, 100% named** (mangled Itanium C++) |
| 14347 | `h1_mp64_ship_dump.exe` | MWR PC target, 41,806 real-.text funcs, 1,993 named |
| 9999  | `2-h1_mp.elf` | **MWR reference, DEBUG build, 98.2% named, asserts carry source path + line** |

⭐ Both references are essentially fully named, and CoD4's mangled names carry
STRUCT TYPES in their signatures (`playerState_s`, `entityState_s`, `clientInfo_t`,
`hudelem_s`, `WeaponDef`, `pmove_t`, `trajectory_t`) — a type library for free.

---

# 1. CLIENT / ENTITY LIMITS

## ⭐ PROVEN — the two caps, each stated by its own engine

    CoD4   Dvar_RegisterInt("ui_maxclients", 32, 1, 64, ...)     MAX = 64
           in __Z7SV_Initv @0x1baa76  (also registers sv_maxclients alongside)

    MWR    "maxClients %d when we expect %d in the vlobby\n", *maxClients, 18
           in CL_GetMaxClients @0x3952e0                          MAX_CLIENTS = 18
           assert in the same function: "*maxClients == MAX_CLIENTS"
           source: D:\h1\code_source\Runtime\client_mp\cl_main_mp.cpp

MWR additionally carries a SEPARATE agent (bot/AI) budget in the same function:
`*maxAgents = 24` / `23` depending on mode. Agents are not clients — do not
conflate them when mapping a CoD4 roster.

## ⭐ THE INVENTORY — every place a limit is enforced, from the debug build

MWR-PS4 is a DEBUG build, so **every bounds check is annotated with an assert that
names the constant and carries its source file and line.** That assert set is an
authoritative "find them all" inventory; hunting for the literal `18` would be
hopeless by comparison.

`re/mwr_client_sites.py` → `re/mwr_client_sites.json`. **1,176 bounded sites:**

    MAX_GENTITIES            817
    MAX_CLIENTS              232
    MAX_CONFIGSTRINGS         72
    MAX_AGENTS                31
    MAX_WEAPONS               13
    MAX_HUDELEMS               6
    MAX_PARTY_CLIENT_COUNT     3
    MAX_SNAPSHOT_ENTITIES      2

Top enforcing source files: `g_scr_main.cpp` (202), `g_client_script_cmd.cpp`
(128), `g_client_script_cmd_mp.cpp` (43), `sv_agent_scr.cpp` (41),
`sv_bot_scr.cpp` (30), `sv_analytics_mp.cpp` (26), `sv_streamsync_mp.cpp` (18).

## ⭐⭐ THE ACTIONABLE SUBSET — 36 sites a DEMO actually executes

Most of the 232 MAX_CLIENTS sites are SERVER or GSC side (`sv_*`, `g_scr_*`) and a
demo never runs them. Filtering to `CL_*` / `CG_* `/ `MSG_*` / `BG_*` leaves **36**.
The ones that decide whether a CoD4 demo parses at all:

| function | source | check |
|---|---|---|
| **`CL_DeltaClient`** | `cl_parse_mp.cpp` | **`newnum doesn't index MAX_CLIENTS`** |
| **`CL_ParseMessage`** | `cl_parse_mp.cpp` | **`newnum doesn't index MAX_CLIENTS`** |
| `CG_SetNextSnap` | `cg_snapshot_mp.cpp` | `clientState->clientIndex doesn't index MAX_CLIENTS` |
| `CG_TransitionSnapshot` | `cg_snapshot_mp.cpp` | `clientState->clientIndex doesn't index MAX_CLIENTS` |
| `CG_ParsePlayerInfos` | `cg_servercmds_mp.cpp` | `foundClientNum doesn't index MAX_CLIENTS` |
| `CG_UpdateScores` | `cg_servercmds_mp.cpp` | `clientIdx doesn't index MAX_CLIENTS` |
| `CG_DeployServerCommandString` | `cg_servercmds_mp.cpp` | `clientNum doesn't index MAX_CLIENTS` |
| `CG_AgentCorpse` | `cg_players_mp.cpp` | `es->clientNum >= MAX_CLIENTS \|\| ...` |
| `CG_DrawOverheadNames`, `CG_UpdatePlayerNames` | `cg_draw_mp.cpp` | `...clientNum doesn't index MAX_CLIENTS` |
| `CG_DrawScoreboard_GetTeamColorIndex` | `cg_scoreboard_mp.cpp` | `cgameGlob->clientNum ...` |
| `CG_MaterialHandleForIndex` | `cg_newDraw_mp.cpp` | `ps->clientNum ...` |
| `CL_GetConfigString`, `CL_GetConfigStringFromGameState` | `cl_cgame_mp.cpp` | `configStringIndex doesn't index MAX_CONFIGSTRINGS` |
| `CL_ConfigstringModified`, `CL_ParseConfigStrings` | | `configstring > MAX_CONFIGSTRINGS` |
| `MSG_ReadDeltaHudElems` | `msg_mp.cpp` | `count == MAX_HUDELEMS_ARCHIVAL \|\| count == MAX_HUDELEMS` |

Plus the `cl_voice.cpp` family (mute/voice, 6 sites) which is cosmetic for playback.

**CONSEQUENCE:** a CoD4 demo whose roster uses client slots ≥ 18 cannot be replayed
by remapping alone — the slots must be COMPACTED into 0..17, consistently, across:
the packet-clients section, every `entityState.clientNum`, the configstring client
block, scoreboard/score updates, and every server command that carries a client
index. A compaction table is therefore a first-class part of the transcode, not an
afterthought.

⚠ **UNRESOLVED and important:** whether a CoD4 demo with >18 *simultaneously
connected* players can be represented at all, or only demos whose live roster
happens to fit. A 64-slot server with 30 players cannot be compacted into 18 without
dropping players. Decide the policy (drop spectators first? drop by distance? refuse?)
before building the transcoder — it changes the data model.

## `MSG_GetPlatformSignature` — worth knowing about

References BOTH `MAX_CLIENTS` and `MAX_CONFIGSTRINGS`. Reads as a protocol
compatibility hash built from the engine's limits. NOT yet decompiled. If it is
used to validate a demo or a connection, changing any limit would invalidate it.

---

# 2. NETFIELDS (the wire format core)

## PROVEN — table sizes, from dumps already in hand

    CoD4  playerState   141 fields, stride 16, layout name*:u32, offset:u32, bits:i16, type:u16, flags:u32
    CoD4  entityState    60 fields, stride 16
    MWR   playerState   252 fields, stride  8, layout offset:u16, bits:i16, type:u16, flags:u16
    MWR   (6 more lists) 71 / 92 / 37 / 7 / 44 / 63

⭐ **Both use an 8-bit `lastChanged` index** — `ceil_log2(141) = ceil_log2(252) = 8`
— so the outer delta framing survives the port unchanged. That is lucky and it is
the single reason a field-level transcode is viable at all.

⛔ But the field SETS are entirely different (141 vs 252), so every field must be
mapped individually. There is no "copy the field stream" shortcut.

⚠ Note CoD4's netfield record carries a NAME pointer; MWR's does not. MWR's names
come from the PS4 build, where `GetPlayerStateNetFields` / `GetEntityStateNetFields`
/ `GetClientStateNetFields` (0x72b5f0 / 0x72b590 / 0x72b620) resolve the tables and
`MSG_GetNetFieldTypeName` (0x7226e0) resolves the `type` codes to names.

## PROVEN — CoD4 delta protocol has NO skip-count

Re-verified from CoD4's own `MSG_ReadDeltaPlayerstate` disassembly (`v12 += 4` /
`--v82` per field): CoD4 iterates fields `0..lastChanged-1` **sequentially**, each
reading its own "changed" bit. The skip-count mechanism in MW3/S2 is a LATER
addition and is not present in IW3. The existing Python decoder is faithful here.

`flags == 2` fields skip the per-field changed bit in CoD4
(`if (*(_BYTE *)(a6 + 12) != 2)` in `MSG_ReadDeltaField`), and the Python
implements it (`delta_field.py:294`).

## The MWR delta surface, all named in PS4 (use these, do not re-derive)

    MSG_ReadDeltaFields       0x728b40     MSG_WriteDeltaFields      0x741920
    MSG_ReadDeltaField        0x7273a0     MSG_WriteDeltaField       0x73fba0
    MSG_ReadDeltaPlayerstate  0x7255d0     MSG_WriteDeltaStruct      0x73d6c0
    MSG_ReadDeltaEntity       0x7246f0     MSG_WriteDeltaClient      0x73db10
    MSG_ReadDeltaClient       0x725330     MSG_WriteDeltaHudElems    0x741d90
    MSG_ReadDeltaStruct       0x725240     MSG_WriteDeltaScoreboard  0x742600
    MSG_ReadDeltaAgent        0x725410     MSG_WriteDeltaOmnvars     0x742d30
    MSG_ReadDeltaHudElems     0x728f40     MSG_SetupNetFieldListsForGame 0x73a090
    MSG_ReadDeltaScoreboard   0x729580     GetEntityStateNetFields   0x72b590
    MSG_ReadDeltaOmnvars      0x729cf0     GetPlayerStateNetFields   0x72b5f0
    MSG_GetNetFieldTypeName   0x7226e0     GetClientStateNetFields   0x72b620
    CL_DeltaEntity            0x39f420     CL_DeltaClient            0x39b320

⭐ MWR has `MSG_ReadDeltaOmnvars` and `MSG_ReadDeltaAgent` — **subsystems CoD4 does
not have at all.** Omnvars are the LUI data channel; agents are AI/bots. A CoD4 demo
carries nothing for either, so the transcode must emit whatever "empty" encoding
each expects rather than omitting the section.

---

# 3. STILL TO MAP (ordered by what blocks playback)

| # | area | status |
|---|---|---|
| 1 | **playerState field↔field map** (141 → 252) | tables dumped both sides; mapping NOT built |
| 2 | **entityState field↔field map** (60 → ?) | CoD4 dumped; MWR list not yet identified among the 7 |
| 3 | **clientState / packet-clients** | MWR reader `MSG_ReadDeltaClient` named; CoD4 side partially done |
| 4 | **client-slot compaction 64→18** | sites inventoried above; policy undecided |
| 5 | **weapon pool** | CoD4 vs MWR names differ; `03_remap_tables.json` is a partial start |
| 6 | **hudelems** | `MAX_HUDELEMS` + `MAX_HUDELEMS_ARCHIVAL` differ; struct not compared |
| 7 | **configstring index bases** | MWR model base = 1240, attach base = 3898, weapons = 3945 (PROVEN earlier); CoD4 CS_MODELS = 830 |
| 8 | **GSC / server commands** | MWR `CG_DeployServerCommandString` is client-side and bounded |
| 9 | **omnvars / agents** | MWR-only; needs an "empty" encoding |

## Method note

Everything in section 1 came from the debug build's own asserts — no probing, no
guessing at literals. Prefer that route for every remaining limit: search the
constant NAME, not the value.

---

# ⭐⭐⭐ LARGE-PLAYER DEMOS — THE CAPS RECLASSIFIED (2026-08-17)

User directive: **go straight to large player demos.** No compaction policy, no
"refuse >18" gate. That reframes the whole problem, and the answer is far better
than the earlier docs implied — because they were scoped to LIVE 64-player
matches (party, matchmaking, server allocations), where demo PLAYBACK needs only
the client-side path.

## The 18-cap is not one thing. It is FIVE things with very different costs.

| class | example | blocks large demos? |
|---|---|---|
| **WIRE encoding** | `entityState.clientNum` **6 bits** | **NO — 6 bits = 0..63 = exactly 64 clients** |
| **release runtime check** | `CL_ParseGamestate` @0x3412ee `if (v11 > 0x11u)` | yes — but ALREADY PATCHED by `maxclients.cpp` |
| **assert-only** | `CL_DeltaClient` "newnum doesn't index MAX_CLIENTS" | **NO — MyAssertHandler, stripped in release** |
| **fixed arrays** | `bgs.clientinfo[18]`, `bgs.characterinfo[42]` | yes — ALREADY RELOCATED to [64] by `cgame_slots.cpp` |
| **dynamic rings** | `cl->parseClients` | **NO — scales automatically** |
| party `[18] * 0x270` | lobby / matchmaking peers | **NOT USED by demo playback** |

## PROVEN — the wire carries 64 clients

    CoD4  entityState.clientNum   7 bits   (0..127)
    MWR   entityState.clientNum   6 bits   (0..63)   <- covers 64 slots exactly
    MWR   playerState.clientNum   6 bits

So the bit width is NOT the blocker. This was the one thing that could have been
fatal (a 5-bit field would cap at 31 and no array resize would help).

## PROVEN — `cl->parseClients` is dynamically allocated, not a fixed [18]

    CL_AllocSnapshotMemory @0x388f50 : Hunk_UserAlloc(hunk, 156 * count, 4)
    CL_CalcSnapshotMemory  @0x388e10 : 244*v16*a2*a3 + 156*v16*a2*a4 + 280832*v16*a2 + ...

`156 = sizeof(clientState_t)`, `244 = sizeof(entityState_s)`. The count comes from
the client/agent counts that `CL_GetMaxClients` supplies, so **raising that value
resizes the ring automatically.** This closes the P0 item `07_client_slot_patch_sites.md`
left open as "Snapshot client delta / clientState rings — TBD".

## PROVEN — only ONE release-build runtime check exists on this path

    MWR-PC sub_3411A0 @0x3412ee    if (v11 > 0x11u)      REAL check, release build
    MWR-PC CL_ParseMessage         no 18-check at all    assert was stripped

Confirmed by contrast: the anchor pass could not name `CL_DeltaClient`,
`CL_ParseGamestate`, `MSG_ReadDelta*` in MWR-PC precisely BECAUSE their assert
strings are absent from the release build — while the same functions are named in
the PS4 debug build. Absence of the string IS the evidence the check is gone.

## ⛔ CORRECTION — the netfield tables are .bss, built at RUNTIME

I tried to read the PS4 netfield registry statically and got all `0xFF`. The
tables at `g_netFieldList` (PS4 `0x1411a18` -> registry `0x14119a0`) are **.bss,
constructed at runtime by `GLOBAL__sub_I_sv_msg_write_mp_cpp` @0x7446e0**. A
static IDB read returns nothing. The named tables must be recovered by EMULATING
that initializer's stores — which a previous session already did.

The registry SHAPE does read correctly and independently reproduces the record in
memory `mwr-netfields-named`:

    [0] EntityState    0xbb42a60   72
    [1] ArchivedEntity 0xbb42ee0  119
    [2] ClientState    0xbb43650   56
    [3] PlayerState    0xbb439d0  271
    [4] Objective      0xbb44ac0    7
    [5] HudElem        0xbb44b30   44

## ⭐ PRIOR WORK IS MUCH FURTHER ALONG THAN THE PLAN DOCS SUGGEST

Two engine components are ALREADY LANDED and are on the current `iwxmvm-base`
branch as well as `demo-only`:

  * `src/client/component/maxclients.cpp` — raises the sv/ui/party dvar maxima
    (opt-in `h1_sv_slots`), scales four fixed SV buffers, and patches the
    gamestate clamp at `0x3412EC` from 17 to N-1
  * `src/client/component/cgame_slots.cpp` — relocates `clientinfo` and
    `characterinfo` into 64-wide pools with writer/reader redirects, syncing the
    stock arrays each frame for readers not yet redirected

And `docs/cod4-transcode-recon.md` (337 KB, on `develop`) is a **nine-area
adversarially-reviewed dossier**: mwr-named-netfields, mwr-named-entityfields,
cod4-dm1-format (verdict SHAKY), cod4-named-netfields, mwr-gamestate, mwr-clock,
mwr-viewmodel-gun, cod4-to-mwr-mapping, no-loopback. READ IT BEFORE ANY NEW RE.

## ⭐ The transcode is a value copy for most fields

From the dossier, PROVEN both sides: **MWR kept CoD4's entire encoder vocabulary**
(-97 DeltaTime, -88 float32-XOR, -87 Angle16, -98 24BitFlag, -94/-93 event-param,
positive N = N-bit int), and `MSG_ReadAngle16`'s scale constant is byte-identical
in both binaries (`0.0054931640625` = 360/65536). Origin, velocity, viewangles,
commandTime and viewHeight therefore need **no unit conversion** — only a
name-mapped field alias.

⚠ But **field ORDER differs completely** (CoD4 idx1 = `viewangles[1]`; MWR idx1 =
`pe.eventSequence`), so raw index/bit copying cannot work. And five fields are
genuinely hard: weapon id, anim indices, bobCycle, movementDir sign, eventParms.

⚠ CoD4 has TWO playerState tables. Use `server_mp/server_mp.h:359`
`playerStateFields[141]` (16-byte records, the MP table a .dm_1 actually uses).
`msg.cpp:70`'s `[143]` (12-byte) is the **cod3-era SP path — a decoy.**

## WHAT ACTUALLY REMAINS for large-player demos

  1. make `CL_GetMaxClients` report the raised count during demo playback, so the
     parseClients/parseEntities rings size correctly
  2. the playerState + entityState field alias tables (141 -> 271 / 60 -> 72)
  3. characterinfo readers still reading cg-inline above 42
  4. LUI scoreboard / minimap 18-wide assumptions
  5. `MSG_GetPlatformSignature` — references MAX_CLIENTS and MAX_CONFIGSTRINGS;
     decompile before changing any limit

---

# 3. .dm_h1 CONTAINER + MESSAGE DECODE (2026-08-17, offline, corpus-driven)

Oracle corpus: `F:/Coding/H1-Mod-Demos/demo-corpus/real/` — 15 real h1-mod demos
(backlot, crash, shipment, showdown, citystreets, bog_summer, farm, vlobby_room),
rescued from the Recycle Bin. The old synthetic attempts are retired to
`_retired/` and are NOT used as a reference (user directive: they were too far off).

## PROVEN — network_data record layout (from h1-mod's own reader)

    [0..4]  u32  serverMessageSequence
    [4..8]  u32  reliableAcknowledge   (bit31 = compression flag)
    [8..]        svc opcode stream, parsed from offset 0

No added container header — the recorder captures `msg.data[0..cursize)` verbatim
and those 8 bytes are the engine's own message preamble.

⛔ CORRECTION: this is NOT "entry readcount" in bits 0..30. That is the `.dm_s2`
container's meaning; `.dm_h1` stores reliableAcknowledge. **The transcoder needs no
readcount fidelity**, and the low 31 bits are written but IGNORED on playback (the
reader overwrites with `clc->reliableSequence`). Only bit31 is consumed.

## PROVEN — MWR message dispatch

`CL_ParseServerMessage` @PS4 0x39bb80 -> `CL_ParseMessage` @0x39bda0.
Opcode = `MSG_ReadBits(msg, 4)`, the FIRST statement of the parse loop (no preamble).
Switch cases: **{0,2,3,4,5,6}** plus an explicit `== 7` (EOF). Anything else ->
"CL_ParseServerMessage: Illegible server message %d". So opcodes >= 8 are ILLEGAL.

    0 gamestate   2 servercmd TEXT   3 servercmd BINARY
    4 table upload  5 matchdata      6 snapshot   7 EOF

## PROVEN — real messages are RAW and snapshot-dominated

backlot.0000: 1023 of 1064 non-zlib messages start with opcode 6, avg body 14 bytes.
Real MWR is a continuous trickle of tiny deltas. Compression is NOT in play for
the bulk of a recording.

## ⭐⭐ PROVEN — CoD4 and MWR use DIFFERENT Huffman TABLES **and** DIFFERENT DECODERS

    MWR   Huff_offsetReceive @PS4 0x71f760 : FLAT LOOKUP TABLE
            table[0x000..0x0FF] = code length for the next 8-bit window
            table[0x100..0x1FF] = symbol
            table[0x200.. ]     = multi-level continuation when length > 8
          g_huffReadData  @PS4 0x1411000   (decode table, 16 KB extracted)
          g_huffWriteData @PS4 0x1410c00   (256 entries, sum 1,708,466)
                          @PC  0x12d4400   -- SAME TABLE in the PC build

    CoD4  Huff_offsetReceive @0x180e10 : classic Q3 POINTER-TREE walk,
          one bit at a time (node = bit ? node->right : node->left)

The MWR table is NOT present in either CoD4 build (byte-exact search).

⛔ This kills the old code's assumption, stated in `mwr/huffman.py`:
"MWR reuses the classic iw3 msg_hData table". **False on both counts** — different
table AND different decoder structure. Any transcode must decompress with CoD4's
tree and recompress with MWR's flat-table encoder.

Extracted: `demo-corpus/tools/mwr_hdata.json` (freq table),
`mwr_huffread.json` (16 KB decode table), `mwr_huff.py` (decoder transcription).

## ⚠ OPEN — the "illegal opcode" message class

3.6% (backlot) / 10% (crash) / 30% (showdown) of non-zlib messages begin with a
4-bit value >= 8, which the engine would reject as illegible. They are NOT
corrupt and NOT random:

  * **100% of them have bit 3 set** (617 messages across 3 demos). Random or
    compressed data would be < 8 about half the time - so the leading bits are
    STRUCTURED.
  * masking `& 7` yields plausible opcodes (0,1,2,4,5,6,7)
  * they recur with a **period of 40 messages** (runs at seq 38, 81, 121, 161,
    201, 241, 281...) ~= every 2 s at 20 Hz
  * consecutive ones share an identical multi-byte TAIL with each other and the
    valid messages share their own tail - same message class, bit-packed
  * MWR's real Huffman decoder does NOT decode them into valid streams either

Hypotheses still open: a leading flag bit before the opcode; a distinct record
class the recorder emits periodically; or a compression path whose framing we
have not identified. DO NOT build the re-encoder until this is resolved - a
decoder that silently mis-handles 3-30% of messages is exactly how the previous
attempt produced 39/39 "converted" files that never reached CA_ACTIVE.

## Tooling (demo-corpus/tools/)

    scan_dm_h1.py     container walk + census + real/synthetic diff
    probe_msgpath.py  which compression path a message takes
    classify_msgs.py  per-message classification, raw vs huffman
    mwr_huff.py       MWR's flat-table Huffman decoder (transcribed)
