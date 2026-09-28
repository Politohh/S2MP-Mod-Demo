/* ===========================================================================
 * clientDemoPlayback_t -- Call of Duty: WWII (S2).  Head exact + shift map.
 * ===========================================================================
 *
 * This is the struct the demo work has been calling "PlaybackData" and poking
 * with raw offsets for months. Global: qword_10F340A0. Accessor:
 * CL_Demo_GetPlaybackData (IDA 0x915DF0) -- but only 15 functions use it; the
 * other 85 read the global directly, which is why re/map_global_struct.py exists.
 *
 * DERIVATION
 *   103 offsets extracted from all 85 functions referencing the global, then
 *   mapped against AW's fully-named clientDemoPlayback_t (74 members).
 *
 * ⭐ THE HEAD IS EXACT -- offsets AND types
 *   Every field from 0..76 matches AW. Two independent corroborations:
 *     - +28 comes back typed FLOAT from S2's own code, matching AW's timeScale;
 *     - +36..+72 is TEN consecutive dword accesses = AW's buttonPressTime[10],
 *       which lands the head at exactly 76 = AW's persistentDataBuffer offset.
 *
 *   Six of these were already derived by hand, and all six were
 *   right: completed@8, overridePause@10, jumpTimeFlag@12, totalBytesRead@24,
 *   timeScale@28, and the byte at +32 that CL_Demo_HandleAction 16/167 toggles --
 *   which AW names hideDemoHud.
 *
 * ⭐⭐ BEYOND THE HEAD THE SHIFT IS A CONSTANT +64000
 *   S2's persistentDataBuffer is 79,356 bytes (the 0x135FC memcpy'd from the demo
 *   header at +0x1C); AW's is 15,356. Difference: exactly 64,000. Everything after
 *   it shifts by that and nothing else changes, confirmed on five independent
 *   offsets -- four of which this project had already measured by hand:
 *
 *     S2 offset    -64000     AW field                     how S2 knew it
 *     ----------------------------------------------------------------------
 *      2176588     2112588    keyFrame[250] + 4            (extracted here)
 *      3336304     3272304    clipState                    "mode: 3=playing,2=ended"
 *      3336316     3272316    clipRecordBuf[2097152]       "a 2 MB buffer"
 *      5433468     5369468    clipRecordBufIndex           "buffer fill level"
 *      5439380     5375380    inside segments[10]          "stride 65704"  <-- and
 *                                                          657040/10 == 65704 exactly
 *
 *   So to name any S2 offset in this region: subtract 64000 and read AW's field.
 *
 * ⭐ THIS REFRAMES the earlier clip-buffer finding
 *   That entry records "a 2 MB in-memory demo buffer with a second reader" at
 *   +3336316, fill level at +5433468, and treats it as a general demo read buffer.
 *   It is AW's **clipRecordBuf** / **clipRecordBufIndex** -- the CLIP RECORDING
 *   buffer. So the "second reader" (sub_919190) is the clip preview/playback path,
 *   not an alternate route for normal demo reading. The measurement was right; the
 *   interpretation was broader than the truth.
 *
 *   Likewise +3336304, recorded as "mode: 3 = playing, 2 = ended, 1 = clip
 *   capture", is **clipState** -- so those values are clip states, which is exactly
 *   consistent with "1 = clip capture" but explains why the numbering looked odd.
 *
 * ⚠ NOT ESTABLISHED: total size. The +64000 shift does NOT hold all the way down --
 *   the measured camera mode at +6297228 and block count at +6096400 do not land on
 *   AW fields at that shift, so there are further insertions deeper in. Do not
 *   extrapolate past the table above without checking.
 * =========================================================================== */

#pragma once
#include <cstdint>

struct clientDemoPlayback_head          /* EXACT for 0..76; see the shift map above */
{
    /* +0  */ std::int32_t  localClientNum;
    /* +4  */ std::uint32_t maxMemoryUsed;
    /* +8  */ bool          completed;                      /* CL_Demo_IsCompleted   */
    /* +9  */ bool          shutdownInProgress;
    /* +10 */ bool          overridePause;                  /* CL_Demo_IsPaused reads
                                                               !this                 */
    /* +11 */ bool          firstDrawFrameEventsProcessed;
    /* +12 */ bool          jumpTimeFlag;                   /* CL_Demo_GetJumpTimeFlag */
    /* +13 */ std::uint8_t  _u13[3];
    /* +16 */ std::int32_t  lastGeneratedKeyframeTime;
    /* +20 */ std::int32_t  lastInvalidatedKeyframe;
    /* +24 */ std::int32_t  totalBytesRead;                 /* the file cursor. This is
                                                               the number that proved
                                                               the priming runaway --
                                                               terminator+2 vs +1    */
    /* +28 */ float         timeScale;                      /* PROVEN: CL_Demo_HandleAction
                                                               20/21/23 step it by 0.1
                                                               and clamp 0.1..4.0    */
    /* +32 */ bool          hideDemoHud;                    /* CL_Demo_HandleAction
                                                               16/167 toggles it     */
    /* +33 */ std::uint8_t  _u33[3];
    /* +36 */ std::int32_t  buttonPressTime[10];            /* ten consecutive dwords */
};

static_assert(sizeof(clientDemoPlayback_head) == 76,
              "head must end where persistentDataBuffer begins");
static_assert(offsetof(clientDemoPlayback_head, completed)      == 8,  "completed");
static_assert(offsetof(clientDemoPlayback_head, overridePause)  == 10, "overridePause");
static_assert(offsetof(clientDemoPlayback_head, jumpTimeFlag)   == 12, "jumpTimeFlag");
static_assert(offsetof(clientDemoPlayback_head, totalBytesRead) == 24, "totalBytesRead");
static_assert(offsetof(clientDemoPlayback_head, timeScale)      == 28, "timeScale");
static_assert(offsetof(clientDemoPlayback_head, hideDemoHud)    == 32, "hideDemoHud");

/* --- fields beyond the head, by S2 offset (AW offset + 64000) ----------------
 *
 *   +79432     keyframeBuf[2097152]      (AW 15432)
 *   +2176584   keyFrame[250]             (AW 2112584)  48 bytes/slot, 12000 total
 *   +2188584   keyframeIndex             (AW 2124584)
 *   +2188588   keyframeBufferIndex       (AW 2124588)
 *   +2188592   keyframeBufferReadIndex   (AW 2124592)
 *   +2188596   baselineKeyframes[32]     (AW 2124596)
 *   +2188724   baselineIndex             (AW 2124724)
 *   +3336304   clipState                 (AW 3272304)
 *   +3336316   clipRecordBuf[2097152]    (AW 3272316)
 *   +5433468   clipRecordBufIndex        (AW 5369468)
 *   +5435012   segments[10]              (AW 5371012)  65704 bytes each
 *
 * The keyframe block is the one this project uses most: 250 slots of 48 bytes,
 * with slot+8 = the demo time and slot+32/+36 = the replay range whose emptiness
 * (a == b) means the jump restores no snapshot.
 * --------------------------------------------------------------------------- */
