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

#ifndef RTP_SENDER_H_

#define RTP_SENDER_H_

#include "RTPBase.h"

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct ANetworkSession;

// An object of this class facilitates sending of media data over an RTP
// channel. The channel is established over a UDP or TCP connection depending
// on which "TransportMode" was chosen. In addition different RTP packetization
// schemes are supported such as "Transport Stream Packets over RTP",
// or "AVC/H.264 encapsulation as specified in RFC 3984 (non-interleaved mode)"
struct RTPSender : public RTPBase, public AHandler {
    enum {
        kWhatInitDone,
        kWhatError,
        kWhatNetworkStall,
        kWhatInformSender,
    };
    RTPSender(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify);

    status_t initAsync(
              const char *remoteHost,
              int32_t remoteRTPPort,
              TransportMode rtpMode,
              int32_t remoteRTCPPort,
              TransportMode rtcpMode,
              int32_t *outLocalRTPPort);

    status_t queueBuffer(
            const sp<ABuffer> &buffer,
            uint8_t packetType,
            PacketizationMode mode);

protected:
    virtual ~RTPSender();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatRTPNotify,
        kWhatRTCPNotify,
    };

    enum {
        kMaxNumTSPacketsPerRTPPacket = (kMaxUDPPacketSize - 12) / 188,
        kMaxHistorySize              = 1024,
        kSourceID                    = 0xdeadbeef,
    };

    sp<ANetworkSession> mNetSession;
    sp<AMessage> mNotify;
    TransportMode mRTPMode;
    TransportMode mRTCPMode;
    int32_t mRTPSessionID;
    int32_t mRTCPSessionID;
    bool mRTPConnected;
    bool mRTCPConnected;

    uint64_t mLastNTPTime;
    uint32_t mLastRTPTime;
    uint32_t mNumRTPSent;
    uint32_t mNumRTPOctetsSent;
    uint32_t mNumSRsSent;

    uint32_t mRTPSeqNo;

    List<sp<ABuffer> > mHistory;
    size_t mHistorySize;

    static uint64_t GetNowNTP();

    status_t queueRawPacket(const sp<ABuffer> &tsPackets, uint8_t packetType);
    status_t queueTSPackets(const sp<ABuffer> &tsPackets, uint8_t packetType);
    status_t queueAVCBuffer(const sp<ABuffer> &accessUnit, uint8_t packetType);

    status_t sendRTPPacket(
            const sp<ABuffer> &packet, bool storeInHistory,
            bool timeValid = false, int64_t timeUs = -1ll);

    void onNetNotify(bool isRTP, const sp<AMessage> &msg);

    status_t onRTCPData(const sp<ABuffer> &data);
    status_t parseReceiverReport(const uint8_t *data, size_t size);
    status_t parseTSFB(const uint8_t *data, size_t size);
    status_t parseAPP(const uint8_t *data, size_t size);

    void notifyInitDone(status_t err);
    void notifyError(status_t err);
    void notifyNetworkStall(size_t numBytesQueued);

    DISALLOW_EVIL_CONSTRUCTORS(RTPSender);
};

}  // namespace android

#endif  // RTP_SENDER_H_
