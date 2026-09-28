/* ===========================================================================
 * clientConnection_t -- Call of Duty: WWII (S2), Steam build.  clc.
 * ===========================================================================
 *
 * The struct this project has been poking with raw offsets since the demo work
 * began. Global: `clc` @ IDA 0x1BD3D00 -- a POINTER, now typed
 * clientConnection_t*. Per-client array, stride 482656.
 *
 * DERIVATION
 *   80 distinct offsets extracted from all 122 functions referencing the global,
 *   by DISASSEMBLY displacements (re/map_global_disasm.py), then mapped against
 *   AW's fully-named clientConnection_t (25 members).
 *
 *   The decompiler route got 11 offsets out of the same 122 functions, because
 *   IDA renders the dominant access form as
 *       v18 = (char *)clc + 482656 * v14 + 262480;
 *   where the stride hides in a variable and the base constant folds into
 *   another. Machine code has no such freedom: `mov [rax+rcx+40260h], edx`
 *   states the displacement outright.
 *
 * ⭐⭐ THE SIZE IS AN INDEPENDENT PROOF, NOT AN ASSUMPTION
 *   Built purely from field offsets, this struct comes to EXACTLY 482,656 bytes
 *   -- the per-client array stride, which was derived separately from the
 *   `482656 * client` term in the access pattern. Two independent routes, same
 *   number. A layout that were wrong anywhere in the middle would not land on it.
 *
 * ⭐ VALIDATION AGAINST THE PROJECT'S OWN HAND-DERIVED OFFSETS
 *   17 of 17 offsets previously measured by hand were recovered
 *   independently by the extractor, including all four heavily-used ones:
 *       +312     reliableAcknowledge      25 hits
 *       +131396  serverCommandSequence    18 hits
 *       +262744  demo file handle         18 hits
 *       +346144  demo read callback       28 hits
 *
 * ⭐⭐ TWO MORE SELF-CONFIRMATIONS, FROM THE DEMO BLOCK
 *   Neither was designed for -- they fell out of the offsets:
 *     262788 + 79356 == 342144, the very next observed field. So
 *       clc+262760 IS the demo file header and it is EXACTLY 79,384 bytes --
 *       which is the value the header's own +0x04 field carries and which
 *       CL_Demo_Play_f validates. The struct reproduces the file format.
 *     342144 + 3984 == 346128, again the next observed field. 3984 is the
 *       footer body tag CL_Demo_ReadFooter requires.
 *
 * ===========================================================================
 * ⭐⭐⭐ THE HEADLINE FINDING: +0x20140 IS AN S2-ONLY FIELD
 * ===========================================================================
 *
 * An earlier investigation recorded this state-ownership failure:
 *
 *     "Our previous implementation wrote a recorded sequence into clc + 0x20140.
 *      IDA subsequently demonstrated that 0x20140 was not the field we had
 *      assumed it was."   ... and later: "Still unproven: 0x20140."
 *
 * It is now proven, and it explains why it could never be identified by analogy:
 * **S2 inserts a dword at +131392 that has NO counterpart in AW (or Q3).** The
 * AW->S2 offset shift is +8 immediately below it and +12 immediately above:
 *
 *     S2 offset   AW offset   shift   field
 *     ----------------------------------------------------------------
 *      131388      131380      +8     serverMessageSequence
 *      131392        --         --    <-- S2-ONLY.  0x20140.
 *      131396      131384      +12    serverCommandSequence
 *      131400      131388      +12    lastExecutedServerCommand
 *      131404      131392      +12    serverCommands[128][1024]
 *
 * So writing a sequence number there was not "the wrong sequence field" -- it was
 * a field with no sequence semantics at all, in a game that had added it. Every
 * attempt to identify it by mapping from a reference build was doomed.
 *
 * The shift is MONOTONIC (differences come from insertions, never removals), so
 * +8 below and +12 above forces exactly one 4-byte insertion between them. It
 * lands on 131392, which the extractor sees 16 times from CL_Disconnect,
 * CL_InitCGame and CL_ParseSnapshot -- so it is real and load-bearing, and its
 * MEANING is still unknown. Left named s2_only_0x20140 rather than guessed.
 *
 * ===========================================================================
 * ⛔ CORRECTED: conflicting notes about +312 / +131388
 * ===========================================================================
 *
 *   the doctrine says   incoming message sequence -> 0x2013C (131388)
 *                       server reliable sequence  -> 0x138   (312)
 *   the demo note says  snapshot sequence -> 0x2013C, message sequence -> 0x138
 *
 * Both cannot hold. Settled from the disassembly, and THE DOCTRINE IS RIGHT:
 *
 *   CL_Demo_ProcessPacket_Type2_3 does, in order:
 *       MSG_ReadLong  ->  [+312]
 *       read [+308], compare against [+312], write [+312]
 *
 *   That is Q3's server-message preamble verbatim:
 *       clc.reliableAcknowledge = MSG_ReadLong(msg);
 *       if (clc.reliableAcknowledge < clc.reliableSequence - MAX_RELIABLE_COMMANDS)
 *           clc.reliableAcknowledge = clc.reliableSequence;
 *   -- and the earlier decode of that gate uses the literal 128, which IS
 *   MAX_RELIABLE_COMMANDS. So +312 is reliableAcknowledge and +308 is
 *   reliableSequence, both "reliable sequences" as the doctrine said.
 *
 *   +131388 is then serverMessageSequence, which is what the doctrine called the
 *   incoming message sequence. The demo note had the pair swapped.
 *
 * ⭐ AND THE OLD demo_game.hpp COMMENT IS WRONG
 *   demo_game.hpp:460-463 records "CLC_RELIABLE_ACKNOWLEDGE (0x20144) and
 *   CLC_SERVER_COMMAND_SEQUENCE are the same field". They are two DIFFERENT
 *   fields 131,084 bytes apart: reliableAcknowledge is at +312 (0x138), and
 *   0x20144 is serverCommandSequence. The CLAMP that was built on that reading
 *   is still harmless -- it clamps serverCommandSequence to itself -- but the
 *   stated reason for it is not true. Do not extend that reasoning.
 *
 * ===========================================================================
 * HOW THE HEAD WAS ANCHORED
 * ===========================================================================
 *
 *   +4 = clientNum          CL_Demo_WriteGameState reads `movsx [r14+4]` and
 *                           writes it as the gamestate's clientNum. AW has
 *                           clientNum@4. Shift 0.
 *   +8 = lastPacketTime     CONFIRMED AFTER THE FACT, once the struct was applied:
 *                           CL_Demo_ProcessPacket_Type2_3 decompiles to
 *                           `v5->lastPacketTime = cls_realtime;`. Stamping a
 *                           realtime value into it is what the name means, so a
 *                           shift-0 inference became behaviour-proven for free.
 *   +304 = checksumFeed     the LAST long CL_Demo_WriteGameState writes --
 *                           exactly where MW3's named twin writes
 *                           CCS_GetChecksum(). AW checksumFeed@296. Shift +8.
 *   +316 = reliableCommands 316 + 131072 == 131388, the next observed field.
 *                           AW@308. Shift +8, consistent with checksumFeed.
 *
 *   The 8-byte insertion therefore sits between +12 and +296, in the region AW
 *   fills with serverAddress / connect state / serverMessage[256]. The extractor
 *   sees NO accesses at all in 44..304, which is what an error-string buffer
 *   touched only by string functions looks like -- so nothing contradicts it,
 *   but the exact split is unknown and that whole region is left as one gap.
 *
 * ⚠ WHAT IS *NOT* MAPPED
 *   Everything past +346160 is one 136,496-byte gap. AW puts netchan,
 *   netchanOutgoingBuffer, netchanIncomingBuffer, OOBProf and the stat-packet
 *   fields there; S2 clearly has equivalents (the extractor sees +346160..+346192,
 *   +425048..+425056 from CL_Demo_FreeCameraMove, and +482376/+482384 from
 *   CL_Disconnect / CL_WritePacket), but the shift is unknown that deep and none
 *   of it has been proven from S2's own code. Do not extrapolate.
 * =========================================================================== */

#pragma once
#include <cstdint>

struct clientConnection_t
{
    /* +0      */ std::int32_t qport;
    /* +4      */ std::int32_t clientNum;                 /* PROVEN: CL_Demo_WriteGameState */
    /* +8      */ std::int32_t lastPacketTime;
    /* +12     */ std::uint8_t serverAddress[16];         /* netadr_t                       */
    /* +28     */ std::int32_t connectLastSendTime;
    /* +32     */ std::int32_t connectPacketCount;        /* Cl_CheckForResend_Connecting   */
    /* +36     */ std::uint8_t _u036_serverMessage[268];  /* AW: serverMessage[256] + the
                                                             8-byte S2 insertion + challenge.
                                                             Split UNKNOWN -- one gap.      */
    /* +304    */ std::int32_t checksumFeed;              /* PROVEN: last long of the demo
                                                             gamestate (MW3 twin: checksum) */
    /* +308    */ std::int32_t reliableSequence;
    /* +312    */ std::int32_t reliableAcknowledge;       /* PROVEN: MSG_ReadLong, then the
                                                             Q3 clamp against
                                                             reliableSequence - 128         */
    /* +316    */ std::uint8_t reliableCommands[131072];  /* [128][1024]; ends exactly at
                                                             serverMessageSequence          */
    /* +131388 */ std::int32_t serverMessageSequence;     /* the doctrine's "incoming
                                                             message sequence", 0x2013C     */
    /* +131392 */ std::int32_t s2_only_0x20140;           /* ⭐ S2-ONLY. No AW/Q3 counterpart.
                                                             MEANING UNKNOWN -- do not write.
                                                             This is the field the old
                                                             implementation corrupted.      */
    /* +131396 */ std::int32_t serverCommandSequence;     /* PROVEN: svc 2/3 both do
                                                             if (x < seq) x = seq           */
    /* +131400 */ std::int32_t lastExecutedServerCommand; /* type-3 packets write it        */
    /* +131404 */ std::uint8_t serverCommands[131072];    /* PROVEN: [128][1024] ring,
                                                             indexed seq & 0x7F             */

    /* ---- demo. All offsets proven from S2's own demo code. ------------------ */
    /* +262476 */ std::uint8_t _u262476[4];
    /* +262480 */ char         demoName[256];             /* sub_911400(clc+262480, 256, ..) */
    /* +262736 */ std::uint8_t _u262736[8];
    /* +262744 */ std::int64_t demoFileHandle;
    /* +262752 */ std::int32_t demoState;                 /* 0 idle, 2 playing              */
    /* +262756 */ std::uint8_t _u262756[4];

    /* +262760 .. +342144 is the .demo FILE HEADER, exactly 79384 bytes -- the
       value demoHeader_headerSize itself carries and CL_Demo_Play_f validates. */
    /* +262760 */ std::int32_t demoHeader_version;        /* must == 29                     */
    /* +262764 */ std::int32_t demoHeader_headerSize;     /* must == 79384                  */
    /* +262768 */ std::int32_t demoHeader_demoClientNum;  /* -> Dvar_SetInt(cl_demo_client) */
    /* +262772 */ std::uint8_t demoHeader_isClipRecording;
    /* +262773 */ std::uint8_t demoHeader_isPrivateMatch; /* file +0x0D; 1 on every demo
                                                             that plays, 0 on public ones   */
    /* +262774 */ std::uint8_t _u262774[2];
    /* +262776 */ std::uint64_t demoHeader_recorderXuid;  /* file +0x10; sub_789870(client)  */
    /* +262784 */ std::int32_t demoHeader_exeMode;        /* must == 5, else Com_Error "428" */
    /* +262788 */ std::uint8_t demoHeader_persistentData[79356];  /* the 0x135FC memcpy      */

    /* +342144 .. +346128 is the FOOTER BODY, exactly 3984 bytes -- the tag
       CL_Demo_ReadFooter requires at body[1]. Map name is at +272 (= 342416).  */
    /* +342144 */ std::uint8_t demoFooterBody[3984];

    /* +346128 */ std::int32_t demoRecord_u346128;        /* StartRecord zeroes it; the
                                                             append path and the keyframe
                                                             writer both use it. UNKNOWN.   */
    /* ⭐ +346132 -- NOT "is recording". CORRECTED 2026-08-17 after the append chain was
       found. CL_Demo_StartRecord sets it to 1; CL_Demo_NoteFirstSnapshotForRecord
       (@0x91C6E0, sole caller CL_ParseSnapshot) CLEARS it on the first snapshot and
       stashes a value into demoFooterBody+8. The per-message append proceeds only when
       it reads 0 -- so it means "recording armed, still waiting for a snapshot to
       anchor to", not "recording". The old name came from an earlier annotation
       rather than from code, which is exactly what the Absolute Rule forbids. */
    /* +346132 */ std::int32_t demoRecordPendingFirstSnapshot;
    /* +346136 */ std::int64_t demoRecordMessageState;    /* the state pointer the append
                                                             trio takes as arg0. Also the
                                                             slot CL_Demo_StartRecord fills
                                                             with sub_912450.               */
    /* +346144 */ std::int64_t demoReadCallback;          /* CL_Demo_Read / sub_919190      */
    /* +346152 */ std::uint8_t demoDisconnectOnEof;       /* the timedemo flag; 0 for a
                                                             plain cl_demo_play             */
    /* +346153 */ std::uint8_t _u346153[7];

    /* +346160 */ std::uint8_t _u346160[136496];          /* netchan + buffers + OOBProf +
                                                             stat packets. UNMAPPED.        */
};

static_assert(sizeof(clientConnection_t) == 482656,
              "must equal the per-client array stride -- this is the layout's own proof");

static_assert(offsetof(clientConnection_t, clientNum)                 == 4,      "clientNum");
static_assert(offsetof(clientConnection_t, checksumFeed)              == 304,    "checksumFeed");
static_assert(offsetof(clientConnection_t, reliableAcknowledge)       == 312,    "0x138");
static_assert(offsetof(clientConnection_t, reliableCommands)          == 316,    "ring");
static_assert(offsetof(clientConnection_t, serverMessageSequence)     == 131388, "0x2013C");
static_assert(offsetof(clientConnection_t, s2_only_0x20140)           == 131392, "0x20140");
static_assert(offsetof(clientConnection_t, serverCommandSequence)     == 131396, "0x20144");
static_assert(offsetof(clientConnection_t, lastExecutedServerCommand) == 131400, "0x20148");
static_assert(offsetof(clientConnection_t, serverCommands)            == 131404, "0x2014C");
static_assert(offsetof(clientConnection_t, demoFileHandle)            == 262744, "demo handle");
static_assert(offsetof(clientConnection_t, demoHeader_version)        == 262760, "hdr");
static_assert(offsetof(clientConnection_t, demoFooterBody)            == 342144, "footer");
static_assert(offsetof(clientConnection_t, demoReadCallback)          == 346144, "read cb");
static_assert(offsetof(clientConnection_t, demoRecordPendingFirstSnapshot) == 346132, "latch");
static_assert(offsetof(clientConnection_t, demoRecordMessageState)    == 346136, "append state");

/* The header block is the .demo file header verbatim, so these hold too: */
static_assert(offsetof(clientConnection_t, demoFooterBody)
            - offsetof(clientConnection_t, demoHeader_version) == 79384,
              "the demo header is exactly headerSize bytes");
static_assert(offsetof(clientConnection_t, demoRecord_u346128)
            - offsetof(clientConnection_t, demoFooterBody) == 3984,
              "the footer body is exactly the 3984 tag");
