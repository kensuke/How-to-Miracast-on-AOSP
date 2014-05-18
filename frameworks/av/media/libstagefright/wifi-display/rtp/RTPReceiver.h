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

#ifndef RTP_RECEIVER_H_

#define RTP_RECEIVER_H_

#include "RTPBase.h"

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct ANetworkSession;

// An object of this class facilitates receiving of media data on an RTP
// channel. The channel is established over a UDP or TCP connection depending
// on which "TransportMode" was chosen. In addition different RTP packetization
// schemes are supported such as "Transport Stream Packets over RTP",
// or "AVC/H.264 encapsulation as specified in RFC 3984 (non-interleaved mode)"
struct RTPReceiver : public RTPBase, public AHandler {
    enum {
        kWhatInitDone,
        kWhatError,
        kWhatAccessUnit,
        kWhatPacketLost,
    };

    enum Flags {
        FLAG_AUTO_CONNECT = 1,
    };
    RTPReceiver(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify,
            uint32_t flags = 0);

    status_t registerPacketType(
            uint8_t packetType, PacketizationMode mode);

    status_t initAsync(
            TransportMode rtpMode,
            TransportMode rtcpMode,
            int32_t *outLocalRTPPort);

    status_t connect(
            const char *remoteHost,
            int32_t remoteRTPPort,
            int32_t remoteRTCPPort);

    status_t informSender(const sp<AMessage> &params);

protected:
    virtual ~RTPReceiver();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatRTPNotify,
        kWhatRTCPNotify,
        kWhatSendRR,
    };

    enum {
        kSourceID                       = 0xdeadbeef,
        kPacketLostAfterUs              = 100000,
        kRequestRetransmissionAfterUs   = -1,
    };

    struct Assembler;
    struct H264Assembler;
    struct Source;
    struct TSAssembler;

    sp<ANetworkSession> mNetSession;
    sp<AMessage> mNotify;
    uint32_t mFlags;
    TransportMode mRTPMode;
    TransportMode mRTCPMode;
    int32_t mRTPSessionID;
    int32_t mRTCPSessionID;
    bool mRTPConnected;
    bool mRTCPConnected;

    int32_t mRTPClientSessionID;  // in TRANSPORT_TCP mode.
    int32_t mRTCPClientSessionID;  // in TRANSPORT_TCP mode.

    KeyedVector<uint8_t, PacketizationMode> mPacketTypes;
    KeyedVector<uint32_t, sp<Source> > mSources;

    void onNetNotify(bool isRTP, const sp<AMessage> &msg);
    status_t onRTPData(const sp<ABuffer> &data);
    status_t onRTCPData(const sp<ABuffer> &data);
    void onSendRR();

    void scheduleSendRR();
    void addSDES(const sp<ABuffer> &buffer);

    void notifyInitDone(status_t err);
    void notifyError(status_t err);
    void notifyPacketLost();

    sp<Assembler> makeAssembler(uint8_t packetType);

    void requestRetransmission(uint32_t senderSSRC, int32_t extSeqNo);

    DISALLOW_EVIL_CONSTRUCTORS(RTPReceiver);
};

}  // namespace android

#endif  // RTP_RECEIVER_H_
