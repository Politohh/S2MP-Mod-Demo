/* ===========================================================================
 * centity_s -- Call of Duty: WWII (S2), Steam build s2x_dump.exe
 * ===========================================================================
 *
 * Derived 2026-08-17 from S2's OWN code, not ported from a reference build.
 *
 * METHOD
 *   CG_GetEntity (IDA 0x13A00) returns a centity*, so every dereference of its
 *   return value is a field access. All 306 callers were decompiled and every
 *   `*(TYPE *)(var + N)` collected: 116 distinct offsets. The highest touched is
 *   +1804, which fits exactly inside the 1808-byte stride and independently
 *   confirms it.
 *
 *   "hits" below = how many of those 306 functions touch that offset. Advanced
 *   Warfare's centity_s (524 bytes, 32-bit PPC) supplied field NAMES and the
 *   region ordering; its OFFSETS do not transfer and were not used.
 *
 * ONLY PROVEN FIELDS ARE NAMED. Everything else is an explicit _unk_ gap, so an
 * unmapped field shows up in a decompile instead of being silently absorbed.
 * That is deliberate -- it is how +1184 was spotted (see below).
 *
 * REGION MAP
 *   0x000 .. 0x490   pose (cpose_t) + prevState (LerpEntityState).
 *                    OWORD run at 1048/1064/1080/1096/1112/1128/1144, 16-byte
 *                    spaced -- trajectory vectors.
 *   0x490 .. 0x6A0   nextState (entityState_s), ~528 bytes (AW's is 256).
 *                    lerp starts at +1340: eFlags there, then OWORDs at
 *                    1356/1372/1388/1404/1420/1436 = the two trajectories.
 *   0x6A0 .. 0x710   centity scalars (AW places `flags` right after nextState).
 *
 * ACCESSOR
 *   centity_s *CG_GetEntity(int localClientNum, unsigned entnum);   // IDA 0x13A00
 *   returns qword_8B0DB18 + 1808 * ((localClientNum << 11) + entnum)
 *
 *   !! DO NOT CALL IT FROM THE MOD !!
 *   It inspects its own RETURN ADDRESS (Arxan). Called from outside the game
 *   image it does not return what a game-side caller gets -- this is what made
 *   the origin read yield inf/3.9e17 and got `player_box.cpp` wrongly written off
 *   (the OFFSET was always right; the POINTER was not).
 *   And you cannot just reproduce the arithmetic either: qword_8B0DB18 is
 *   ENCRYPTED (reads as 0xF5A16DA2226E8E90 live).
 * =========================================================================== */

#pragma once
#include <cstdint>

struct centity_s                       /* sizeof == 1808 (0x710) */
{
    /* 0x000 */ std::uint8_t  _unk_000[48];   /* pose head. Cf AW cpose_t: eType@2,
                                                 cullIn@3, isRagdoll@4, handles@8/12.
                                                 Byte fields at 2/3/4/5/10 and dwords
                                                 at 8/12 are touched; roles unproven. */
    /* 0x030 */ float         origin[3];      /* +48   PROVEN: vec3 copy in sub_45540
                                                       via sub_AF9B70(cent + 48, out) */
    /* 0x03C */ std::uint8_t  _unk_03C[1108]; /* rest of pose + prevState.
                                                 Notable unmapped: vec3-shaped dword
                                                 triple at 64/68/72 (15/17/14 hits);
                                                 qword at 24 (11 hits); OWORD
                                                 trajectory run at 1048..1144. */
    /* 0x490 */ std::int16_t  number;         /* +1168 PROVEN: entity number, 94 hits */
    /* 0x492 */ std::uint16_t otherEntityNum; /* +1170 PROVEN: 17 hits */
    /* 0x494 */ std::uint8_t  _unk_494[6];
    /* 0x49A */ std::uint8_t  eType;          /* +1178 PROVEN: 108 hits */
    /* 0x49B */ std::uint8_t  _unk_49B[22];   /* contains a real field at +1184: read as
                                                 a 48-bit mask, (q >> 13) & (1 << n) &
                                                 0xFFFFFFFFFFFF. AW has partBits[8] /
                                                 clientMask / threatMask in this role;
                                                 WHICH one is unproven, so unnamed. */
    /* 0x4B1 */ char          clientNum;      /* +1201 PROVEN: 18 hits.
                                                 !! 1-BASED -- the engine does `- 1` !! */
    /* 0x4B2 */ std::uint8_t  _unk_4B2[114];
    /* 0x524 */ std::int32_t  weapon;         /* +1316 PROVEN: CG_GetWeaponName passes
                                                       cent + 1316 as a Weapon* */
    /* 0x528 */ std::uint8_t  _unk_528[20];
    /* 0x53C */ std::int32_t  lerp_eFlags;    /* +1340 PROVEN: 28 hits. Start of
                                                       nextState.lerp; trajectories
                                                       follow as OWORDs. */
    /* 0x540 */ std::uint8_t  _unk_540[352];
    /* 0x6A0 */ std::uint8_t  flags;          /* +1696 PROVEN: bit0 = in use.
                                                       197 hits -- the most-referenced
                                                       field in the whole struct. */
    /* 0x6A1 */ std::uint8_t  _unk_6A1[111];
};

static_assert(sizeof(centity_s) == 1808, "centity_s must match the S2 array stride");
static_assert(offsetof(centity_s, origin)      == 48,   "origin");
static_assert(offsetof(centity_s, number)      == 1168, "number");
static_assert(offsetof(centity_s, eType)       == 1178, "eType");
static_assert(offsetof(centity_s, clientNum)   == 1201, "clientNum");
static_assert(offsetof(centity_s, weapon)      == 1316, "weapon");
static_assert(offsetof(centity_s, lerp_eFlags) == 1340, "lerp_eFlags");
static_assert(offsetof(centity_s, flags)       == 1696, "flags");

/* Entity array geometry (from CG_GetEntity's ~15 identical return sites):
 *     stride              1808 bytes
 *     entities per client 2048  (localClientNum << 11)
 * Note the DObj side uses a DIFFERENT count -- sub_45540 bounds entity numbers
 * against 2113 for word_AC4EAD0[entnum + 2113 * localClientNum]. Do not conflate
 * the two.
 */
