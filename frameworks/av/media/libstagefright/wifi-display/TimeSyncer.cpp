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

//#define LOG_NEBUG 0
#define LOG_TAG "TimeSyncer"
#include <utils/Log.h>

#include "TimeSyncer.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>
#include <media/stagefright/Utils.h>

namespace android {

TimeSyncer::TimeSyncer(
        const sp<ANetworkSession> &netSession, const sp<AMessage> &notify)
    : mNetSession(netSession),
      mNotify(notify),
      mIsServer(false),
      mConnected(false),
      mUDPSession(0),
      mSeqNo(0),
      mTotalTimeUs(0.0),
      mPendingT1(0ll),
      mTimeoutGeneration(0) {
}

TimeSyncer::~TimeSyncer() {
}

void TimeSyncer::startServer(unsigned localPort) {
    sp<AMessage> msg = new AMessage(kWhatStartServer, id());
    msg->setInt32("localPort", localPort);
    msg->post();
}

void TimeSyncer::startClient(const char *remoteHost, unsigned remotePort) {
    sp<AMessage> msg = new AMessage(kWhatStartClient, id());
    msg->setString("remoteHost", remoteHost);
    msg->setInt32("remotePort", remotePort);
    msg->post();
}

void TimeSyncer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStartClient:
        {
            AString remoteHost;
            CHECK(msg->findString("remoteHost", &remoteHost));

            int32_t remotePort;
            CHECK(msg->findInt32("remotePort", &remotePort));

            sp<AMessage> notify = new AMessage(kWhatUDPNotify, id());

            CHECK_EQ((status_t)OK,
                     mNetSession->createUDPSession(
                         0 /* localPort */,
                         remoteHost.c_str(),
                         remotePort,
                         notify,
                         &mUDPSession));

            postSendPacket();
            break;
        }

        case kWhatStartServer:
        {
            mIsServer = true;

            int32_t localPort;
            CHECK(msg->findInt32("localPort", &localPort));

            sp<AMessage> notify = new AMessage(kWhatUDPNotify, id());

            CHECK_EQ((status_t)OK,
                     mNetSession->createUDPSession(
                         localPort, notify, &mUDPSession));

            break;
        }

        case kWhatSendPacket:
        {
            if (mHistory.size() == 0) {
                ALOGI("starting batch");
            }

            TimeInfo ti;
            memset(&ti, 0, sizeof(ti));

            ti.mT1 = ALooper::GetNowUs();

            CHECK_EQ((status_t)OK,
                     mNetSession->sendRequest(
                         mUDPSession, &ti, sizeof(ti)));

            mPendingT1 = ti.mT1;
            postTimeout();
            break;
        }

        case kWhatTimedOut:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mTimeoutGeneration) {
                break;
            }

            ALOGI("timed out, sending another request");
            postSendPacket();
            break;
        }

        case kWhatUDPNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    mNetSession->destroySession(sessionID);

                    cancelTimeout();

                    notifyError(err);
                    break;
                }

                case ANetworkSession::kWhatDatagram:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    sp<ABuffer> packet;
                    CHECK(msg->findBuffer("data", &packet));

                    int64_t arrivalTimeUs;
                    CHECK(packet->meta()->findInt64(
                                "arrivalTimeUs", &arrivalTimeUs));

                    CHECK_EQ(packet->size(), sizeof(TimeInfo));

                    TimeInfo *ti = (TimeInfo *)packet->data();

                    if (mIsServer) {
                        if (!mConnected) {
                            AString fromAddr;
                            CHECK(msg->findString("fromAddr", &fromAddr));

                            int32_t fromPort;
                            CHECK(msg->findInt32("fromPort", &fromPort));

                            CHECK_EQ((status_t)OK,
                                     mNetSession->connectUDPSession(
                                         mUDPSession, fromAddr.c_str(), fromPort));

                            mConnected = true;
                        }

                        ti->mT2 = arrivalTimeUs;
                        ti->mT3 = ALooper::GetNowUs();

                        CHECK_EQ((status_t)OK,
                                 mNetSession->sendRequest(
                                     mUDPSession, ti, sizeof(*ti)));
                    } else {
                        if (ti->mT1 != mPendingT1) {
                            break;
                        }

                        cancelTimeout();
                        mPendingT1 = 0;

                        ti->mT4 = arrivalTimeUs;

                        // One way delay for a packet to travel from client
                        // to server or back (assumed to be the same either way).
                        int64_t delay =
                            (ti->mT2 - ti->mT1 + ti->mT4 - ti->mT3) / 2;

                        // Offset between the client clock (T1, T4) and the
                        // server clock (T2, T3) timestamps.
                        int64_t offset =
                            (ti->mT2 - ti->mT1 - ti->mT4 + ti->mT3) / 2;

                        mHistory.push_back(*ti);

                        ALOGV("delay = %lld us,\toffset %lld us",
                               delay,
                               offset);

                        if (mHistory.size() < kNumPacketsPerBatch) {
                            postSendPacket(1000000ll / 30);
                        } else {
                            notifyOffset();

                            ALOGI("batch done");

                            mHistory.clear();
                            postSendPacket(kBatchDelayUs);
                        }
                    }
                    break;
                }

                default:
                    TRESPASS();
            }

            break;
        }

        default:
            TRESPASS();
    }
}

void TimeSyncer::postSendPacket(int64_t delayUs) {
    (new AMessage(kWhatSendPacket, id()))->post(delayUs);
}

void TimeSyncer::postTimeout() {
    sp<AMessage> msg = new AMessage(kWhatTimedOut, id());
    msg->setInt32("generation", mTimeoutGeneration);
    msg->post(kTimeoutDelayUs);
}

void TimeSyncer::cancelTimeout() {
    ++mTimeoutGeneration;
}

void TimeSyncer::notifyError(status_t err) {
    if (mNotify == NULL) {
        looper()->stop();
        return;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

// static
int TimeSyncer::CompareRountripTime(const TimeInfo *ti1, const TimeInfo *ti2) {
    int64_t rt1 = ti1->mT4 - ti1->mT1;
    int64_t rt2 = ti2->mT4 - ti2->mT1;

    if (rt1 < rt2) {
        return -1;
    } else if (rt1 > rt2) {
        return 1;
    }

    return 0;
}

void TimeSyncer::notifyOffset() {
    mHistory.sort(CompareRountripTime);

    int64_t sum = 0ll;
    size_t count = 0;

    // Only consider the third of the information associated with the best
    // (smallest) roundtrip times.
    for (size_t i = 0; i < mHistory.size() / 3; ++i) {
        const TimeInfo *ti = &mHistory[i];

#if 0
        // One way delay for a packet to travel from client
        // to server or back (assumed to be the same either way).
        int64_t delay =
            (ti->mT2 - ti->mT1 + ti->mT4 - ti->mT3) / 2;
#endif

        // Offset between the client clock (T1, T4) and the
        // server clock (T2, T3) timestamps.
        int64_t offset =
            (ti->mT2 - ti->mT1 - ti->mT4 + ti->mT3) / 2;

        ALOGV("(%d) RT: %lld us, offset: %lld us",
              i, ti->mT4 - ti->mT1, offset);

        sum += offset;
        ++count;
    }

    if (mNotify == NULL) {
        ALOGI("avg. offset is %lld", sum / count);
        return;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatTimeOffset);
    notify->setInt64("offset", sum / count);
    notify->post();
}

}  // namespace android
