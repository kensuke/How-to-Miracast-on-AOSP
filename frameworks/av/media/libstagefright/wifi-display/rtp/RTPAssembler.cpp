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

//#define LOG_NDEBUG 0
#define LOG_TAG "RTPAssembler"
#include <utils/Log.h>

#include "RTPAssembler.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

RTPReceiver::Assembler::Assembler(const sp<AMessage> &notify)
    : mNotify(notify) {
}

void RTPReceiver::Assembler::postAccessUnit(
        const sp<ABuffer> &accessUnit, bool followsDiscontinuity) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", RTPReceiver::kWhatAccessUnit);
    notify->setBuffer("accessUnit", accessUnit);
    notify->setInt32("followsDiscontinuity", followsDiscontinuity);
    notify->post();
}
////////////////////////////////////////////////////////////////////////////////

RTPReceiver::TSAssembler::TSAssembler(const sp<AMessage> &notify)
    : Assembler(notify),
      mSawDiscontinuity(false) {
}

void RTPReceiver::TSAssembler::signalDiscontinuity() {
    mSawDiscontinuity = true;
}

status_t RTPReceiver::TSAssembler::processPacket(const sp<ABuffer> &packet) {
    int32_t rtpTime;
    CHECK(packet->meta()->findInt32("rtp-time", &rtpTime));

    packet->meta()->setInt64("timeUs", (rtpTime * 100ll) / 9);

    postAccessUnit(packet, mSawDiscontinuity);

    if (mSawDiscontinuity) {
        mSawDiscontinuity = false;
    }

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

RTPReceiver::H264Assembler::H264Assembler(const sp<AMessage> &notify)
    : Assembler(notify),
      mState(0),
      mIndicator(0),
      mNALType(0),
      mAccessUnitRTPTime(0) {
}

void RTPReceiver::H264Assembler::signalDiscontinuity() {
    reset();
}

status_t RTPReceiver::H264Assembler::processPacket(const sp<ABuffer> &packet) {
    status_t err = internalProcessPacket(packet);

    if (err != OK) {
        reset();
    }

    return err;
}

status_t RTPReceiver::H264Assembler::internalProcessPacket(
        const sp<ABuffer> &packet) {
    const uint8_t *data = packet->data();
    size_t size = packet->size();

    switch (mState) {
        case 0:
        {
            if (size < 1 || (data[0] & 0x80)) {
                ALOGV("Malformed H264 RTP packet (empty or F-bit set)");
                return ERROR_MALFORMED;
            }

            unsigned nalType = data[0] & 0x1f;
            if (nalType >= 1 && nalType <= 23) {
                addSingleNALUnit(packet);
                ALOGV("added single NAL packet");
            } else if (nalType == 28) {
                // FU-A
                unsigned indicator = data[0];
                CHECK((indicator & 0x1f) == 28);

                if (size < 2) {
                    ALOGV("Malformed H264 FU-A packet (single byte)");
                    return ERROR_MALFORMED;
                }

                if (!(data[1] & 0x80)) {
                    ALOGV("Malformed H264 FU-A packet (no start bit)");
                    return ERROR_MALFORMED;
                }

                mIndicator = data[0];
                mNALType = data[1] & 0x1f;
                uint32_t nri = (data[0] >> 5) & 3;

                clearAccumulator();

                uint8_t byte = mNALType | (nri << 5);
                appendToAccumulator(&byte, 1);
                appendToAccumulator(data + 2, size - 2);

                int32_t rtpTime;
                CHECK(packet->meta()->findInt32("rtp-time", &rtpTime));
                mAccumulator->meta()->setInt32("rtp-time", rtpTime);

                if (data[1] & 0x40) {
                    // Huh? End bit also set on the first buffer.
                    addSingleNALUnit(mAccumulator);
                    clearAccumulator();

                    ALOGV("added FU-A");
                    break;
                }

                mState = 1;
            } else if (nalType == 24) {
                // STAP-A

                status_t err = addSingleTimeAggregationPacket(packet);
                if (err != OK) {
                    return err;
                }
            } else {
                ALOGV("Malformed H264 packet (unknown type %d)", nalType);
                return ERROR_UNSUPPORTED;
            }
            break;
        }

        case 1:
        {
            if (size < 2
                    || data[0] != mIndicator
                    || (data[1] & 0x1f) != mNALType
                    || (data[1] & 0x80)) {
                ALOGV("Malformed H264 FU-A packet (indicator, "
                      "type or start bit mismatch)");

                return ERROR_MALFORMED;
            }

            appendToAccumulator(data + 2, size - 2);

            if (data[1] & 0x40) {
                addSingleNALUnit(mAccumulator);

                clearAccumulator();
                mState = 0;

                ALOGV("added FU-A");
            }
            break;
        }

        default:
            TRESPASS();
    }

    int32_t marker;
    CHECK(packet->meta()->findInt32("M", &marker));

    if (marker) {
        flushAccessUnit();
    }

    return OK;
}

void RTPReceiver::H264Assembler::reset() {
    mNALUnits.clear();

    clearAccumulator();
    mState = 0;
}

void RTPReceiver::H264Assembler::clearAccumulator() {
    if (mAccumulator != NULL) {
        // XXX Too expensive.
        mAccumulator.clear();
    }
}

void RTPReceiver::H264Assembler::appendToAccumulator(
        const void *data, size_t size) {
    if (mAccumulator == NULL) {
        mAccumulator = new ABuffer(size);
        memcpy(mAccumulator->data(), data, size);
        return;
    }

    if (mAccumulator->size() + size > mAccumulator->capacity()) {
        sp<ABuffer> buf = new ABuffer(mAccumulator->size() + size);
        memcpy(buf->data(), mAccumulator->data(), mAccumulator->size());
        buf->setRange(0, mAccumulator->size());

        int32_t rtpTime;
        if (mAccumulator->meta()->findInt32("rtp-time", &rtpTime)) {
            buf->meta()->setInt32("rtp-time", rtpTime);
        }

        mAccumulator = buf;
    }

    memcpy(mAccumulator->data() + mAccumulator->size(), data, size);
    mAccumulator->setRange(0, mAccumulator->size() + size);
}

void RTPReceiver::H264Assembler::addSingleNALUnit(const sp<ABuffer> &packet) {
    if (mNALUnits.empty()) {
        int32_t rtpTime;
        CHECK(packet->meta()->findInt32("rtp-time", &rtpTime));

        mAccessUnitRTPTime = rtpTime;
    }

    mNALUnits.push_back(packet);
}

void RTPReceiver::H264Assembler::flushAccessUnit() {
    if (mNALUnits.empty()) {
        return;
    }

    size_t totalSize = 0;
    for (List<sp<ABuffer> >::iterator it = mNALUnits.begin();
            it != mNALUnits.end(); ++it) {
        totalSize += 4 + (*it)->size();
    }

    sp<ABuffer> accessUnit = new ABuffer(totalSize);
    size_t offset = 0;
    for (List<sp<ABuffer> >::iterator it = mNALUnits.begin();
            it != mNALUnits.end(); ++it) {
        const sp<ABuffer> nalUnit = *it;

        memcpy(accessUnit->data() + offset, "\x00\x00\x00\x01", 4);

        memcpy(accessUnit->data() + offset + 4,
               nalUnit->data(),
               nalUnit->size());

        offset += 4 + nalUnit->size();
    }

    mNALUnits.clear();

    accessUnit->meta()->setInt64("timeUs", mAccessUnitRTPTime * 100ll / 9ll);
    postAccessUnit(accessUnit, false /* followsDiscontinuity */);
}

status_t RTPReceiver::H264Assembler::addSingleTimeAggregationPacket(
        const sp<ABuffer> &packet) {
    const uint8_t *data = packet->data();
    size_t size = packet->size();

    if (size < 3) {
        ALOGV("Malformed H264 STAP-A packet (too small)");
        return ERROR_MALFORMED;
    }

    int32_t rtpTime;
    CHECK(packet->meta()->findInt32("rtp-time", &rtpTime));

    ++data;
    --size;
    while (size >= 2) {
        size_t nalSize = (data[0] << 8) | data[1];

        if (size < nalSize + 2) {
            ALOGV("Malformed H264 STAP-A packet (incomplete NAL unit)");
            return ERROR_MALFORMED;
        }

        sp<ABuffer> unit = new ABuffer(nalSize);
        memcpy(unit->data(), &data[2], nalSize);

        unit->meta()->setInt32("rtp-time", rtpTime);

        addSingleNALUnit(unit);

        data += 2 + nalSize;
        size -= 2 + nalSize;
    }

    if (size != 0) {
        ALOGV("Unexpected padding at end of STAP-A packet.");
    }

    ALOGV("added STAP-A");

    return OK;
}

}  // namespace android

