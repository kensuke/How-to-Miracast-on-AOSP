/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TS_PACKETIZER_H_

#define TS_PACKETIZER_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Vector.h>

namespace android {

struct ABuffer;
struct AMessage;

// Forms the packets of a transport stream given access units.
// Emits metadata tables (PAT and PMT) and timestamp stream (PCR) based
// on flags.
struct TSPacketizer : public RefBase {
    enum {
        EMIT_HDCP20_DESCRIPTOR = 1,
        EMIT_HDCP21_DESCRIPTOR = 2,
    };
    TSPacketizer(uint32_t flags);

    // Returns trackIndex or error.
    ssize_t addTrack(const sp<AMessage> &format);

    enum {
        EMIT_PAT_AND_PMT                = 1,
        EMIT_PCR                        = 2,
        IS_ENCRYPTED                    = 4,
        PREPEND_SPS_PPS_TO_IDR_FRAMES   = 8,
    };
    status_t packetize(
            size_t trackIndex, const sp<ABuffer> &accessUnit,
            sp<ABuffer> *packets,
            uint32_t flags,
            const uint8_t *PES_private_data, size_t PES_private_data_len,
            size_t numStuffingBytes = 0);

    status_t extractCSDIfNecessary(size_t trackIndex);

    // XXX to be removed once encoder config option takes care of this for
    // encrypted mode.
    sp<ABuffer> prependCSD(
            size_t trackIndex, const sp<ABuffer> &accessUnit) const;

protected:
    virtual ~TSPacketizer();

private:
    enum {
        kPID_PMT = 0x100,
        kPID_PCR = 0x1000,
    };

    struct Track;

    uint32_t mFlags;
    Vector<sp<Track> > mTracks;

    Vector<sp<ABuffer> > mProgramInfoDescriptors;

    unsigned mPATContinuityCounter;
    unsigned mPMTContinuityCounter;

    uint32_t mCrcTable[256];

    void initCrcTable();
    uint32_t crc32(const uint8_t *start, size_t size) const;

    DISALLOW_EVIL_CONSTRUCTORS(TSPacketizer);
};

}  // namespace android

#endif  // TS_PACKETIZER_H_

