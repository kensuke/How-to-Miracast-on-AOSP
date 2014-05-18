/*
 * Copyright 2013, The Android Open Source Project
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

#ifndef RTP_ASSEMBLER_H_

#define RTP_ASSEMBLER_H_

#include "RTPReceiver.h"

namespace android {

// A helper class to reassemble the payload of RTP packets into access
// units depending on the packetization scheme.
struct RTPReceiver::Assembler : public RefBase {
    Assembler(const sp<AMessage> &notify);

    virtual void signalDiscontinuity() = 0;
    virtual status_t processPacket(const sp<ABuffer> &packet) = 0;

protected:
    virtual ~Assembler() {}

    void postAccessUnit(
            const sp<ABuffer> &accessUnit, bool followsDiscontinuity);

private:
    sp<AMessage> mNotify;

    DISALLOW_EVIL_CONSTRUCTORS(Assembler);
};

struct RTPReceiver::TSAssembler : public RTPReceiver::Assembler {
    TSAssembler(const sp<AMessage> &notify);

    virtual void signalDiscontinuity();
    virtual status_t processPacket(const sp<ABuffer> &packet);

private:
    bool mSawDiscontinuity;

    DISALLOW_EVIL_CONSTRUCTORS(TSAssembler);
};

struct RTPReceiver::H264Assembler : public RTPReceiver::Assembler {
    H264Assembler(const sp<AMessage> &notify);

    virtual void signalDiscontinuity();
    virtual status_t processPacket(const sp<ABuffer> &packet);

private:
    int32_t mState;

    uint8_t mIndicator;
    uint8_t mNALType;

    sp<ABuffer> mAccumulator;

    List<sp<ABuffer> > mNALUnits;
    int32_t mAccessUnitRTPTime;

    status_t internalProcessPacket(const sp<ABuffer> &packet);

    void addSingleNALUnit(const sp<ABuffer> &packet);
    status_t addSingleTimeAggregationPacket(const sp<ABuffer> &packet);

    void flushAccessUnit();

    void clearAccumulator();
    void appendToAccumulator(const void *data, size_t size);

    void reset();

    DISALLOW_EVIL_CONSTRUCTORS(H264Assembler);
};

}  // namespace android

#endif  // RTP_ASSEMBLER_H_

