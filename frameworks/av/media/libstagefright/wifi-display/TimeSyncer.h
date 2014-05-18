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

#ifndef TIME_SYNCER_H_

#define TIME_SYNCER_H_

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ANetworkSession;

/*
   TimeSyncer allows us to synchronize time between a client and a server.
   The client sends a UDP packet containing its send-time to the server,
   the server sends that packet back to the client amended with information
   about when it was received as well as the time the reply was sent back.
   Finally the client receives the reply and has now enough information to
   compute the clock offset between client and server assuming that packet
   exchange is symmetric, i.e. time for a packet client->server and
   server->client is roughly equal.
   This exchange is repeated a number of times and the average offset computed
   over the 30% of packets that had the lowest roundtrip times.
   The offset is determined every 10 secs to account for slight differences in
   clock frequency.
*/
struct TimeSyncer : public AHandler {
    enum {
        kWhatError,
        kWhatTimeOffset,
    };
    TimeSyncer(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify);

    void startServer(unsigned localPort);
    void startClient(const char *remoteHost, unsigned remotePort);

protected:
    virtual ~TimeSyncer();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatStartServer,
        kWhatStartClient,
        kWhatUDPNotify,
        kWhatSendPacket,
        kWhatTimedOut,
    };

    struct TimeInfo {
        int64_t mT1;  // client timestamp at send
        int64_t mT2;  // server timestamp at receive
        int64_t mT3;  // server timestamp at send
        int64_t mT4;  // client timestamp at receive
    };

    enum {
        kNumPacketsPerBatch = 30,
    };
    static const int64_t kTimeoutDelayUs = 500000ll;
    static const int64_t kBatchDelayUs = 60000000ll;  // every minute

    sp<ANetworkSession> mNetSession;
    sp<AMessage> mNotify;

    bool mIsServer;
    bool mConnected;
    int32_t mUDPSession;
    uint32_t mSeqNo;
    double mTotalTimeUs;

    Vector<TimeInfo> mHistory;

    int64_t mPendingT1;
    int32_t mTimeoutGeneration;

    void postSendPacket(int64_t delayUs = 0ll);

    void postTimeout();
    void cancelTimeout();

    void notifyError(status_t err);
    void notifyOffset();

    static int CompareRountripTime(const TimeInfo *ti1, const TimeInfo *ti2);

    DISALLOW_EVIL_CONSTRUCTORS(TimeSyncer);
};

}  // namespace android

#endif  // TIME_SYNCER_H_
