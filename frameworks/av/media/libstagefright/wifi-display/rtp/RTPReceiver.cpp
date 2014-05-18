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
#define LOG_TAG "RTPReceiver"
#include <utils/Log.h>

#include "RTPAssembler.h"
#include "RTPReceiver.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

#define TRACK_PACKET_LOSS       0

namespace android {

////////////////////////////////////////////////////////////////////////////////

struct RTPReceiver::Source : public AHandler {
    Source(RTPReceiver *receiver, uint32_t ssrc);

    void onPacketReceived(uint16_t seq, const sp<ABuffer> &buffer);

    void addReportBlock(uint32_t ssrc, const sp<ABuffer> &buf);

protected:
    virtual ~Source();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatRetransmit,
        kWhatDeclareLost,
    };

    static const uint32_t kMinSequential = 2;
    static const uint32_t kMaxDropout = 3000;
    static const uint32_t kMaxMisorder = 100;
    static const uint32_t kRTPSeqMod = 1u << 16;
    static const int64_t kReportIntervalUs = 10000000ll;

    RTPReceiver *mReceiver;
    uint32_t mSSRC;
    bool mFirst;
    uint16_t mMaxSeq;
    uint32_t mCycles;
    uint32_t mBaseSeq;
    uint32_t mReceived;
    uint32_t mExpectedPrior;
    uint32_t mReceivedPrior;

    int64_t mFirstArrivalTimeUs;
    int64_t mFirstRTPTimeUs;

    // Ordered by extended seq number.
    List<sp<ABuffer> > mPackets;

    enum StatusBits {
        STATUS_DECLARED_LOST            = 1,
        STATUS_REQUESTED_RETRANSMISSION = 2,
        STATUS_ARRIVED_LATE             = 4,
    };
#if TRACK_PACKET_LOSS
    KeyedVector<int32_t, uint32_t> mLostPackets;
#endif

    void modifyPacketStatus(int32_t extSeqNo, uint32_t mask);

    int32_t mAwaitingExtSeqNo;
    bool mRequestedRetransmission;

    int32_t mActivePacketType;
    sp<Assembler> mActiveAssembler;

    int64_t mNextReportTimeUs;

    int32_t mNumDeclaredLost;
    int32_t mNumDeclaredLostPrior;

    int32_t mRetransmitGeneration;
    int32_t mDeclareLostGeneration;
    bool mDeclareLostTimerPending;

    void queuePacket(const sp<ABuffer> &packet);
    void dequeueMore();

    sp<ABuffer> getNextPacket();
    void resync();

    void postRetransmitTimer(int64_t delayUs);
    void postDeclareLostTimer(int64_t delayUs);
    void cancelTimers();

    DISALLOW_EVIL_CONSTRUCTORS(Source);
};

////////////////////////////////////////////////////////////////////////////////

RTPReceiver::Source::Source(RTPReceiver *receiver, uint32_t ssrc)
    : mReceiver(receiver),
      mSSRC(ssrc),
      mFirst(true),
      mMaxSeq(0),
      mCycles(0),
      mBaseSeq(0),
      mReceived(0),
      mExpectedPrior(0),
      mReceivedPrior(0),
      mFirstArrivalTimeUs(-1ll),
      mFirstRTPTimeUs(-1ll),
      mAwaitingExtSeqNo(-1),
      mRequestedRetransmission(false),
      mActivePacketType(-1),
      mNextReportTimeUs(-1ll),
      mNumDeclaredLost(0),
      mNumDeclaredLostPrior(0),
      mRetransmitGeneration(0),
      mDeclareLostGeneration(0),
      mDeclareLostTimerPending(false) {
}

RTPReceiver::Source::~Source() {
}

void RTPReceiver::Source::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRetransmit:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mRetransmitGeneration) {
                break;
            }

            mRequestedRetransmission = true;
            mReceiver->requestRetransmission(mSSRC, mAwaitingExtSeqNo);

            modifyPacketStatus(
                    mAwaitingExtSeqNo, STATUS_REQUESTED_RETRANSMISSION);
            break;
        }

        case kWhatDeclareLost:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mDeclareLostGeneration) {
                break;
            }

            cancelTimers();

            ALOGV("Lost packet extSeqNo %d %s",
                  mAwaitingExtSeqNo,
                  mRequestedRetransmission ? "*" : "");

            mRequestedRetransmission = false;
            if (mActiveAssembler != NULL) {
                mActiveAssembler->signalDiscontinuity();
            }

            modifyPacketStatus(mAwaitingExtSeqNo, STATUS_DECLARED_LOST);

            // resync();
            ++mAwaitingExtSeqNo;
            ++mNumDeclaredLost;

            mReceiver->notifyPacketLost();

            dequeueMore();
            break;
        }

        default:
            TRESPASS();
    }
}

void RTPReceiver::Source::onPacketReceived(
        uint16_t seq, const sp<ABuffer> &buffer) {
    if (mFirst) {
        buffer->setInt32Data(mCycles | seq);
        queuePacket(buffer);

        mFirst = false;
        mBaseSeq = seq;
        mMaxSeq = seq;
        ++mReceived;
        return;
    }

    uint16_t udelta = seq - mMaxSeq;

    if (udelta < kMaxDropout) {
        // In order, with permissible gap.

        if (seq < mMaxSeq) {
            // Sequence number wrapped - count another 64K cycle
            mCycles += kRTPSeqMod;
        }

        mMaxSeq = seq;

        ++mReceived;
    } else if (udelta <= kRTPSeqMod - kMaxMisorder) {
        // The sequence number made a very large jump
        return;
    } else {
        // Duplicate or reordered packet.
    }

    buffer->setInt32Data(mCycles | seq);
    queuePacket(buffer);
}

void RTPReceiver::Source::queuePacket(const sp<ABuffer> &packet) {
    int32_t newExtendedSeqNo = packet->int32Data();

    if (mFirstArrivalTimeUs < 0ll) {
        mFirstArrivalTimeUs = ALooper::GetNowUs();

        uint32_t rtpTime;
        CHECK(packet->meta()->findInt32("rtp-time", (int32_t *)&rtpTime));

        mFirstRTPTimeUs = (rtpTime * 100ll) / 9ll;
    }

    if (mAwaitingExtSeqNo >= 0 && newExtendedSeqNo < mAwaitingExtSeqNo) {
        // We're no longer interested in these. They're old.
        ALOGV("dropping stale extSeqNo %d", newExtendedSeqNo);

        modifyPacketStatus(newExtendedSeqNo, STATUS_ARRIVED_LATE);
        return;
    }

    if (mPackets.empty()) {
        mPackets.push_back(packet);
        dequeueMore();
        return;
    }

    List<sp<ABuffer> >::iterator firstIt = mPackets.begin();
    List<sp<ABuffer> >::iterator it = --mPackets.end();
    for (;;) {
        int32_t extendedSeqNo = (*it)->int32Data();

        if (extendedSeqNo == newExtendedSeqNo) {
            // Duplicate packet.
            return;
        }

        if (extendedSeqNo < newExtendedSeqNo) {
            // Insert new packet after the one at "it".
            mPackets.insert(++it, packet);
            break;
        }

        if (it == firstIt) {
            // Insert new packet before the first existing one.
            mPackets.insert(it, packet);
            break;
        }

        --it;
    }

    dequeueMore();
}

void RTPReceiver::Source::dequeueMore() {
    int64_t nowUs = ALooper::GetNowUs();
    if (mNextReportTimeUs < 0ll || nowUs >= mNextReportTimeUs) {
        if (mNextReportTimeUs >= 0ll) {
            uint32_t expected = (mMaxSeq | mCycles) - mBaseSeq + 1;

            uint32_t expectedInterval = expected - mExpectedPrior;
            mExpectedPrior = expected;

            uint32_t receivedInterval = mReceived - mReceivedPrior;
            mReceivedPrior = mReceived;

            int64_t lostInterval =
                (int64_t)expectedInterval - (int64_t)receivedInterval;

            int32_t declaredLostInterval =
                mNumDeclaredLost - mNumDeclaredLostPrior;

            mNumDeclaredLostPrior = mNumDeclaredLost;

            if (declaredLostInterval > 0) {
                ALOGI("lost %lld packets (%.2f %%), declared %d lost\n",
                      lostInterval,
                      100.0f * lostInterval / expectedInterval,
                      declaredLostInterval);
            }
        }

        mNextReportTimeUs = nowUs + kReportIntervalUs;

#if TRACK_PACKET_LOSS
        for (size_t i = 0; i < mLostPackets.size(); ++i) {
            int32_t key = mLostPackets.keyAt(i);
            uint32_t value = mLostPackets.valueAt(i);

            AString status;
            if (value & STATUS_REQUESTED_RETRANSMISSION) {
                status.append("retrans ");
            }
            if (value & STATUS_ARRIVED_LATE) {
                status.append("arrived-late ");
            }
            ALOGI("Packet %d declared lost %s", key, status.c_str());
        }
#endif
    }

    sp<ABuffer> packet;
    while ((packet = getNextPacket()) != NULL) {
        if (mDeclareLostTimerPending) {
            cancelTimers();
        }

        CHECK_GE(mAwaitingExtSeqNo, 0);
#if TRACK_PACKET_LOSS
        mLostPackets.removeItem(mAwaitingExtSeqNo);
#endif

        int32_t packetType;
        CHECK(packet->meta()->findInt32("PT", &packetType));

        if (packetType != mActivePacketType) {
            mActiveAssembler = mReceiver->makeAssembler(packetType);
            mActivePacketType = packetType;
        }

        if (mActiveAssembler != NULL) {
            status_t err = mActiveAssembler->processPacket(packet);
            if (err != OK) {
                ALOGV("assembler returned error %d", err);
            }
        }

        ++mAwaitingExtSeqNo;
    }

    if (mDeclareLostTimerPending) {
        return;
    }

    if (mPackets.empty()) {
        return;
    }

    CHECK_GE(mAwaitingExtSeqNo, 0);

    const sp<ABuffer> &firstPacket = *mPackets.begin();

    uint32_t rtpTime;
    CHECK(firstPacket->meta()->findInt32(
                "rtp-time", (int32_t *)&rtpTime));


    int64_t rtpUs = (rtpTime * 100ll) / 9ll;

    int64_t maxArrivalTimeUs =
        mFirstArrivalTimeUs + rtpUs - mFirstRTPTimeUs;

    nowUs = ALooper::GetNowUs();

    CHECK_LT(mAwaitingExtSeqNo, firstPacket->int32Data());

    ALOGV("waiting for %d, comparing against %d, %lld us left",
          mAwaitingExtSeqNo,
          firstPacket->int32Data(),
          maxArrivalTimeUs - nowUs);

    postDeclareLostTimer(maxArrivalTimeUs + kPacketLostAfterUs);

    if (kRequestRetransmissionAfterUs > 0ll) {
        postRetransmitTimer(
                maxArrivalTimeUs + kRequestRetransmissionAfterUs);
    }
}

sp<ABuffer> RTPReceiver::Source::getNextPacket() {
    if (mPackets.empty()) {
        return NULL;
    }

    int32_t extSeqNo = (*mPackets.begin())->int32Data();

    if (mAwaitingExtSeqNo < 0) {
        mAwaitingExtSeqNo = extSeqNo;
    } else if (extSeqNo != mAwaitingExtSeqNo) {
        return NULL;
    }

    sp<ABuffer> packet = *mPackets.begin();
    mPackets.erase(mPackets.begin());

    return packet;
}

void RTPReceiver::Source::resync() {
    mAwaitingExtSeqNo = -1;
}

void RTPReceiver::Source::addReportBlock(
        uint32_t ssrc, const sp<ABuffer> &buf) {
    uint32_t extMaxSeq = mMaxSeq | mCycles;
    uint32_t expected = extMaxSeq - mBaseSeq + 1;

    int64_t lost = (int64_t)expected - (int64_t)mReceived;
    if (lost > 0x7fffff) {
        lost = 0x7fffff;
    } else if (lost < -0x800000) {
        lost = -0x800000;
    }

    uint32_t expectedInterval = expected - mExpectedPrior;
    mExpectedPrior = expected;

    uint32_t receivedInterval = mReceived - mReceivedPrior;
    mReceivedPrior = mReceived;

    int64_t lostInterval = expectedInterval - receivedInterval;

    uint8_t fractionLost;
    if (expectedInterval == 0 || lostInterval <=0) {
        fractionLost = 0;
    } else {
        fractionLost = (lostInterval << 8) / expectedInterval;
    }

    uint8_t *ptr = buf->data() + buf->size();

    ptr[0] = ssrc >> 24;
    ptr[1] = (ssrc >> 16) & 0xff;
    ptr[2] = (ssrc >> 8) & 0xff;
    ptr[3] = ssrc & 0xff;

    ptr[4] = fractionLost;

    ptr[5] = (lost >> 16) & 0xff;
    ptr[6] = (lost >> 8) & 0xff;
    ptr[7] = lost & 0xff;

    ptr[8] = extMaxSeq >> 24;
    ptr[9] = (extMaxSeq >> 16) & 0xff;
    ptr[10] = (extMaxSeq >> 8) & 0xff;
    ptr[11] = extMaxSeq & 0xff;

    // XXX TODO:

    ptr[12] = 0x00;  // interarrival jitter
    ptr[13] = 0x00;
    ptr[14] = 0x00;
    ptr[15] = 0x00;

    ptr[16] = 0x00;  // last SR
    ptr[17] = 0x00;
    ptr[18] = 0x00;
    ptr[19] = 0x00;

    ptr[20] = 0x00;  // delay since last SR
    ptr[21] = 0x00;
    ptr[22] = 0x00;
    ptr[23] = 0x00;

    buf->setRange(buf->offset(), buf->size() + 24);
}

////////////////////////////////////////////////////////////////////////////////

RTPReceiver::RTPReceiver(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        uint32_t flags)
    : mNetSession(netSession),
      mNotify(notify),
      mFlags(flags),
      mRTPMode(TRANSPORT_UNDEFINED),
      mRTCPMode(TRANSPORT_UNDEFINED),
      mRTPSessionID(0),
      mRTCPSessionID(0),
      mRTPConnected(false),
      mRTCPConnected(false),
      mRTPClientSessionID(0),
      mRTCPClientSessionID(0) {
}

RTPReceiver::~RTPReceiver() {
    if (mRTCPClientSessionID != 0) {
        mNetSession->destroySession(mRTCPClientSessionID);
        mRTCPClientSessionID = 0;
    }

    if (mRTPClientSessionID != 0) {
        mNetSession->destroySession(mRTPClientSessionID);
        mRTPClientSessionID = 0;
    }

    if (mRTCPSessionID != 0) {
        mNetSession->destroySession(mRTCPSessionID);
        mRTCPSessionID = 0;
    }

    if (mRTPSessionID != 0) {
        mNetSession->destroySession(mRTPSessionID);
        mRTPSessionID = 0;
    }
}

status_t RTPReceiver::initAsync(
        TransportMode rtpMode,
        TransportMode rtcpMode,
        int32_t *outLocalRTPPort) {
    if (mRTPMode != TRANSPORT_UNDEFINED
            || rtpMode == TRANSPORT_UNDEFINED
            || rtpMode == TRANSPORT_NONE
            || rtcpMode == TRANSPORT_UNDEFINED) {
        return INVALID_OPERATION;
    }

    CHECK_NE(rtpMode, TRANSPORT_TCP_INTERLEAVED);
    CHECK_NE(rtcpMode, TRANSPORT_TCP_INTERLEAVED);

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());

    sp<AMessage> rtcpNotify;
    if (rtcpMode != TRANSPORT_NONE) {
        rtcpNotify = new AMessage(kWhatRTCPNotify, id());
    }

    CHECK_EQ(mRTPSessionID, 0);
    CHECK_EQ(mRTCPSessionID, 0);

    int32_t localRTPPort;

    struct in_addr ifaceAddr;
    ifaceAddr.s_addr = INADDR_ANY;

    for (;;) {
        localRTPPort = PickRandomRTPPort();

        status_t err;
        if (rtpMode == TRANSPORT_UDP) {
            err = mNetSession->createUDPSession(
                    localRTPPort,
                    rtpNotify,
                    &mRTPSessionID);
        } else {
            CHECK_EQ(rtpMode, TRANSPORT_TCP);
            err = mNetSession->createTCPDatagramSession(
                    ifaceAddr,
                    localRTPPort,
                    rtpNotify,
                    &mRTPSessionID);
        }

        if (err != OK) {
            continue;
        }

        if (rtcpMode == TRANSPORT_NONE) {
            break;
        } else if (rtcpMode == TRANSPORT_UDP) {
            err = mNetSession->createUDPSession(
                    localRTPPort + 1,
                    rtcpNotify,
                    &mRTCPSessionID);
        } else {
            CHECK_EQ(rtpMode, TRANSPORT_TCP);
            err = mNetSession->createTCPDatagramSession(
                    ifaceAddr,
                    localRTPPort + 1,
                    rtcpNotify,
                    &mRTCPSessionID);
        }

        if (err == OK) {
            break;
        }

        mNetSession->destroySession(mRTPSessionID);
        mRTPSessionID = 0;
    }

    mRTPMode = rtpMode;
    mRTCPMode = rtcpMode;
    *outLocalRTPPort = localRTPPort;

    return OK;
}

status_t RTPReceiver::connect(
        const char *remoteHost, int32_t remoteRTPPort, int32_t remoteRTCPPort) {
    status_t err;

    if (mRTPMode == TRANSPORT_UDP) {
        CHECK(!mRTPConnected);

        err = mNetSession->connectUDPSession(
                mRTPSessionID, remoteHost, remoteRTPPort);

        if (err != OK) {
            notifyInitDone(err);
            return err;
        }

        ALOGI("connectUDPSession RTP successful.");

        mRTPConnected = true;
    }

    if (mRTCPMode == TRANSPORT_UDP) {
        CHECK(!mRTCPConnected);

        err = mNetSession->connectUDPSession(
                mRTCPSessionID, remoteHost, remoteRTCPPort);

        if (err != OK) {
            notifyInitDone(err);
            return err;
        }

        scheduleSendRR();

        ALOGI("connectUDPSession RTCP successful.");

        mRTCPConnected = true;
    }

    if (mRTPConnected
            && (mRTCPConnected || mRTCPMode == TRANSPORT_NONE)) {
        notifyInitDone(OK);
    }

    return OK;
}

status_t RTPReceiver::informSender(const sp<AMessage> &params) {
    if (!mRTCPConnected) {
        return INVALID_OPERATION;
    }

    int64_t avgLatencyUs;
    CHECK(params->findInt64("avgLatencyUs", &avgLatencyUs));

    int64_t maxLatencyUs;
    CHECK(params->findInt64("maxLatencyUs", &maxLatencyUs));

    sp<ABuffer> buf = new ABuffer(28);

    uint8_t *ptr = buf->data();
    ptr[0] = 0x80 | 0;
    ptr[1] = 204;  // APP
    ptr[2] = 0;

    CHECK((buf->size() % 4) == 0u);
    ptr[3] = (buf->size() / 4) - 1;

    ptr[4] = kSourceID >> 24;  // SSRC
    ptr[5] = (kSourceID >> 16) & 0xff;
    ptr[6] = (kSourceID >> 8) & 0xff;
    ptr[7] = kSourceID & 0xff;
    ptr[8] = 'l';
    ptr[9] = 'a';
    ptr[10] = 't';
    ptr[11] = 'e';

    ptr[12] = avgLatencyUs >> 56;
    ptr[13] = (avgLatencyUs >> 48) & 0xff;
    ptr[14] = (avgLatencyUs >> 40) & 0xff;
    ptr[15] = (avgLatencyUs >> 32) & 0xff;
    ptr[16] = (avgLatencyUs >> 24) & 0xff;
    ptr[17] = (avgLatencyUs >> 16) & 0xff;
    ptr[18] = (avgLatencyUs >> 8) & 0xff;
    ptr[19] = avgLatencyUs & 0xff;

    ptr[20] = maxLatencyUs >> 56;
    ptr[21] = (maxLatencyUs >> 48) & 0xff;
    ptr[22] = (maxLatencyUs >> 40) & 0xff;
    ptr[23] = (maxLatencyUs >> 32) & 0xff;
    ptr[24] = (maxLatencyUs >> 24) & 0xff;
    ptr[25] = (maxLatencyUs >> 16) & 0xff;
    ptr[26] = (maxLatencyUs >> 8) & 0xff;
    ptr[27] = maxLatencyUs & 0xff;

    mNetSession->sendRequest(mRTCPSessionID, buf->data(), buf->size());

    return OK;
}

void RTPReceiver::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRTPNotify:
        case kWhatRTCPNotify:
            onNetNotify(msg->what() == kWhatRTPNotify, msg);
            break;

        case kWhatSendRR:
        {
            onSendRR();
            break;
        }

        default:
            TRESPASS();
    }
}

void RTPReceiver::onNetNotify(bool isRTP, const sp<AMessage> &msg) {
    int32_t reason;
    CHECK(msg->findInt32("reason", &reason));

    switch (reason) {
        case ANetworkSession::kWhatError:
        {
            int32_t sessionID;
            CHECK(msg->findInt32("sessionID", &sessionID));

            int32_t err;
            CHECK(msg->findInt32("err", &err));

            int32_t errorOccuredDuringSend;
            CHECK(msg->findInt32("send", &errorOccuredDuringSend));

            AString detail;
            CHECK(msg->findString("detail", &detail));

            ALOGE("An error occurred during %s in session %d "
                  "(%d, '%s' (%s)).",
                  errorOccuredDuringSend ? "send" : "receive",
                  sessionID,
                  err,
                  detail.c_str(),
                  strerror(-err));

            mNetSession->destroySession(sessionID);

            if (sessionID == mRTPSessionID) {
                mRTPSessionID = 0;
            } else if (sessionID == mRTCPSessionID) {
                mRTCPSessionID = 0;
            } else if (sessionID == mRTPClientSessionID) {
                mRTPClientSessionID = 0;
            } else if (sessionID == mRTCPClientSessionID) {
                mRTCPClientSessionID = 0;
            }

            if (!mRTPConnected
                    || (mRTCPMode != TRANSPORT_NONE && !mRTCPConnected)) {
                notifyInitDone(err);
                break;
            }

            notifyError(err);
            break;
        }

        case ANetworkSession::kWhatDatagram:
        {
            sp<ABuffer> data;
            CHECK(msg->findBuffer("data", &data));

            if (isRTP) {
                if (mFlags & FLAG_AUTO_CONNECT) {
                    AString fromAddr;
                    CHECK(msg->findString("fromAddr", &fromAddr));

                    int32_t fromPort;
                    CHECK(msg->findInt32("fromPort", &fromPort));

                    CHECK_EQ((status_t)OK,
                             connect(
                                 fromAddr.c_str(), fromPort, fromPort + 1));

                    mFlags &= ~FLAG_AUTO_CONNECT;
                }

                onRTPData(data);
            } else {
                onRTCPData(data);
            }
            break;
        }

        case ANetworkSession::kWhatClientConnected:
        {
            int32_t sessionID;
            CHECK(msg->findInt32("sessionID", &sessionID));

            if (isRTP) {
                CHECK_EQ(mRTPMode, TRANSPORT_TCP);

                if (mRTPClientSessionID != 0) {
                    // We only allow a single client connection.
                    mNetSession->destroySession(sessionID);
                    sessionID = 0;
                    break;
                }

                mRTPClientSessionID = sessionID;
                mRTPConnected = true;
            } else {
                CHECK_EQ(mRTCPMode, TRANSPORT_TCP);

                if (mRTCPClientSessionID != 0) {
                    // We only allow a single client connection.
                    mNetSession->destroySession(sessionID);
                    sessionID = 0;
                    break;
                }

                mRTCPClientSessionID = sessionID;
                mRTCPConnected = true;
            }

            if (mRTPConnected
                    && (mRTCPConnected || mRTCPMode == TRANSPORT_NONE)) {
                notifyInitDone(OK);
            }
            break;
        }
    }
}

void RTPReceiver::notifyInitDone(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatInitDone);
    notify->setInt32("err", err);
    notify->post();
}

void RTPReceiver::notifyError(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

void RTPReceiver::notifyPacketLost() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatPacketLost);
    notify->post();
}

status_t RTPReceiver::onRTPData(const sp<ABuffer> &buffer) {
    size_t size = buffer->size();
    if (size < 12) {
        // Too short to be a valid RTP header.
        return ERROR_MALFORMED;
    }

    const uint8_t *data = buffer->data();

    if ((data[0] >> 6) != 2) {
        // Unsupported version.
        return ERROR_UNSUPPORTED;
    }

    if (data[0] & 0x20) {
        // Padding present.

        size_t paddingLength = data[size - 1];

        if (paddingLength + 12 > size) {
            // If we removed this much padding we'd end up with something
            // that's too short to be a valid RTP header.
            return ERROR_MALFORMED;
        }

        size -= paddingLength;
    }

    int numCSRCs = data[0] & 0x0f;

    size_t payloadOffset = 12 + 4 * numCSRCs;

    if (size < payloadOffset) {
        // Not enough data to fit the basic header and all the CSRC entries.
        return ERROR_MALFORMED;
    }

    if (data[0] & 0x10) {
        // Header eXtension present.

        if (size < payloadOffset + 4) {
            // Not enough data to fit the basic header, all CSRC entries
            // and the first 4 bytes of the extension header.

            return ERROR_MALFORMED;
        }

        const uint8_t *extensionData = &data[payloadOffset];

        size_t extensionLength =
            4 * (extensionData[2] << 8 | extensionData[3]);

        if (size < payloadOffset + 4 + extensionLength) {
            return ERROR_MALFORMED;
        }

        payloadOffset += 4 + extensionLength;
    }

    uint32_t srcId = U32_AT(&data[8]);
    uint32_t rtpTime = U32_AT(&data[4]);
    uint16_t seqNo = U16_AT(&data[2]);

    sp<AMessage> meta = buffer->meta();
    meta->setInt32("ssrc", srcId);
    meta->setInt32("rtp-time", rtpTime);
    meta->setInt32("PT", data[1] & 0x7f);
    meta->setInt32("M", data[1] >> 7);

    buffer->setRange(payloadOffset, size - payloadOffset);

    ssize_t index = mSources.indexOfKey(srcId);
    sp<Source> source;
    if (index < 0) {
        source = new Source(this, srcId);
        looper()->registerHandler(source);

        mSources.add(srcId, source);
    } else {
        source = mSources.valueAt(index);
    }

    source->onPacketReceived(seqNo, buffer);

    return OK;
}

status_t RTPReceiver::onRTCPData(const sp<ABuffer> &data) {
    ALOGI("onRTCPData");
    return OK;
}

void RTPReceiver::addSDES(const sp<ABuffer> &buffer) {
    uint8_t *data = buffer->data() + buffer->size();
    data[0] = 0x80 | 1;
    data[1] = 202;  // SDES
    data[4] = kSourceID >> 24;  // SSRC
    data[5] = (kSourceID >> 16) & 0xff;
    data[6] = (kSourceID >> 8) & 0xff;
    data[7] = kSourceID & 0xff;

    size_t offset = 8;

    data[offset++] = 1;  // CNAME

    AString cname = "stagefright@somewhere";
    data[offset++] = cname.size();

    memcpy(&data[offset], cname.c_str(), cname.size());
    offset += cname.size();

    data[offset++] = 6;  // TOOL

    AString tool = "stagefright/1.0";
    data[offset++] = tool.size();

    memcpy(&data[offset], tool.c_str(), tool.size());
    offset += tool.size();

    data[offset++] = 0;

    if ((offset % 4) > 0) {
        size_t count = 4 - (offset % 4);
        switch (count) {
            case 3:
                data[offset++] = 0;
            case 2:
                data[offset++] = 0;
            case 1:
                data[offset++] = 0;
        }
    }

    size_t numWords = (offset / 4) - 1;
    data[2] = numWords >> 8;
    data[3] = numWords & 0xff;

    buffer->setRange(buffer->offset(), buffer->size() + offset);
}

void RTPReceiver::scheduleSendRR() {
    (new AMessage(kWhatSendRR, id()))->post(5000000ll);
}

void RTPReceiver::onSendRR() {
    sp<ABuffer> buf = new ABuffer(kMaxUDPPacketSize);
    buf->setRange(0, 0);

    uint8_t *ptr = buf->data();
    ptr[0] = 0x80 | 0;
    ptr[1] = 201;  // RR
    ptr[2] = 0;
    ptr[3] = 1;
    ptr[4] = kSourceID >> 24;  // SSRC
    ptr[5] = (kSourceID >> 16) & 0xff;
    ptr[6] = (kSourceID >> 8) & 0xff;
    ptr[7] = kSourceID & 0xff;

    buf->setRange(0, 8);

    size_t numReportBlocks = 0;
    for (size_t i = 0; i < mSources.size(); ++i) {
        uint32_t ssrc = mSources.keyAt(i);
        sp<Source> source = mSources.valueAt(i);

        if (numReportBlocks > 31 || buf->size() + 24 > buf->capacity()) {
            // Cannot fit another report block.
            break;
        }

        source->addReportBlock(ssrc, buf);
        ++numReportBlocks;
    }

    ptr[0] |= numReportBlocks;  // 5 bit

    size_t sizeInWordsMinus1 = 1 + 6 * numReportBlocks;
    ptr[2] = sizeInWordsMinus1 >> 8;
    ptr[3] = sizeInWordsMinus1 & 0xff;

    buf->setRange(0, (sizeInWordsMinus1 + 1) * 4);

    addSDES(buf);

    mNetSession->sendRequest(mRTCPSessionID, buf->data(), buf->size());

    scheduleSendRR();
}

status_t RTPReceiver::registerPacketType(
        uint8_t packetType, PacketizationMode mode) {
    mPacketTypes.add(packetType, mode);

    return OK;
}

sp<RTPReceiver::Assembler> RTPReceiver::makeAssembler(uint8_t packetType) {
    ssize_t index = mPacketTypes.indexOfKey(packetType);
    if (index < 0) {
        return NULL;
    }

    PacketizationMode mode = mPacketTypes.valueAt(index);

    switch (mode) {
        case PACKETIZATION_NONE:
        case PACKETIZATION_TRANSPORT_STREAM:
            return new TSAssembler(mNotify);

        case PACKETIZATION_H264:
            return new H264Assembler(mNotify);

        default:
            return NULL;
    }
}

void RTPReceiver::requestRetransmission(uint32_t senderSSRC, int32_t extSeqNo) {
    int32_t blp = 0;

    sp<ABuffer> buf = new ABuffer(16);
    buf->setRange(0, 0);

    uint8_t *ptr = buf->data();
    ptr[0] = 0x80 | 1;  // generic NACK
    ptr[1] = 205;  // TSFB
    ptr[2] = 0;
    ptr[3] = 3;
    ptr[8] = (senderSSRC >> 24) & 0xff;
    ptr[9] = (senderSSRC >> 16) & 0xff;
    ptr[10] = (senderSSRC >> 8) & 0xff;
    ptr[11] = (senderSSRC & 0xff);
    ptr[8] = (kSourceID >> 24) & 0xff;
    ptr[9] = (kSourceID >> 16) & 0xff;
    ptr[10] = (kSourceID >> 8) & 0xff;
    ptr[11] = (kSourceID & 0xff);
    ptr[12] = (extSeqNo >> 8) & 0xff;
    ptr[13] = (extSeqNo & 0xff);
    ptr[14] = (blp >> 8) & 0xff;
    ptr[15] = (blp & 0xff);

    buf->setRange(0, 16);

     mNetSession->sendRequest(mRTCPSessionID, buf->data(), buf->size());
}

void RTPReceiver::Source::modifyPacketStatus(int32_t extSeqNo, uint32_t mask) {
#if TRACK_PACKET_LOSS
    ssize_t index = mLostPackets.indexOfKey(extSeqNo);
    if (index < 0) {
        mLostPackets.add(extSeqNo, mask);
    } else {
        mLostPackets.editValueAt(index) |= mask;
    }
#endif
}

void RTPReceiver::Source::postRetransmitTimer(int64_t timeUs) {
    int64_t delayUs = timeUs - ALooper::GetNowUs();
    sp<AMessage> msg = new AMessage(kWhatRetransmit, id());
    msg->setInt32("generation", mRetransmitGeneration);
    msg->post(delayUs);
}

void RTPReceiver::Source::postDeclareLostTimer(int64_t timeUs) {
    CHECK(!mDeclareLostTimerPending);
    mDeclareLostTimerPending = true;

    int64_t delayUs = timeUs - ALooper::GetNowUs();
    sp<AMessage> msg = new AMessage(kWhatDeclareLost, id());
    msg->setInt32("generation", mDeclareLostGeneration);
    msg->post(delayUs);
}

void RTPReceiver::Source::cancelTimers() {
    ++mRetransmitGeneration;
    ++mDeclareLostGeneration;
    mDeclareLostTimerPending = false;
}

}  // namespace android

