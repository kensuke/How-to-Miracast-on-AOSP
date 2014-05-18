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
#define LOG_TAG "nettest"
#include <utils/Log.h>

#include "TimeSyncer.h"

#include <binder/ProcessState.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/stagefright/Utils.h>

namespace android {

struct TestHandler : public AHandler {
    TestHandler(const sp<ANetworkSession> &netSession);

    void listen(int32_t port);
    void connect(const char *host, int32_t port);

protected:
    virtual ~TestHandler();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kTimeSyncerPort = 8123,
    };

    enum {
        kWhatListen,
        kWhatConnect,
        kWhatTimeSyncerNotify,
        kWhatNetNotify,
        kWhatSendMore,
        kWhatStop,
    };

    sp<ANetworkSession> mNetSession;
    sp<TimeSyncer> mTimeSyncer;

    int32_t mServerSessionID;
    int32_t mSessionID;

    int64_t mTimeOffsetUs;
    bool mTimeOffsetValid;

    int32_t mCounter;

    int64_t mMaxDelayMs;

    void dumpDelay(int32_t counter, int64_t delayMs);

    DISALLOW_EVIL_CONSTRUCTORS(TestHandler);
};

TestHandler::TestHandler(const sp<ANetworkSession> &netSession)
    : mNetSession(netSession),
      mServerSessionID(0),
      mSessionID(0),
      mTimeOffsetUs(-1ll),
      mTimeOffsetValid(false),
      mCounter(0),
      mMaxDelayMs(-1ll) {
}

TestHandler::~TestHandler() {
}

void TestHandler::listen(int32_t port) {
    sp<AMessage> msg = new AMessage(kWhatListen, id());
    msg->setInt32("port", port);
    msg->post();
}

void TestHandler::connect(const char *host, int32_t port) {
    sp<AMessage> msg = new AMessage(kWhatConnect, id());
    msg->setString("host", host);
    msg->setInt32("port", port);
    msg->post();
}

void TestHandler::dumpDelay(int32_t counter, int64_t delayMs) {
    static const int64_t kMinDelayMs = 0;
    static const int64_t kMaxDelayMs = 300;

    const char *kPattern = "########################################";
    size_t kPatternSize = strlen(kPattern);

    int n = (kPatternSize * (delayMs - kMinDelayMs))
                / (kMaxDelayMs - kMinDelayMs);

    if (n < 0) {
        n = 0;
    } else if ((size_t)n > kPatternSize) {
        n = kPatternSize;
    }

    if (delayMs > mMaxDelayMs) {
        mMaxDelayMs = delayMs;
    }

    ALOGI("[%d] (%4lld ms / %4lld ms) %s",
          counter,
          delayMs,
          mMaxDelayMs,
          kPattern + kPatternSize - n);
}

void TestHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatListen:
        {
            sp<AMessage> notify = new AMessage(kWhatTimeSyncerNotify, id());
            mTimeSyncer = new TimeSyncer(mNetSession, notify);
            looper()->registerHandler(mTimeSyncer);

            notify = new AMessage(kWhatNetNotify, id());

            int32_t port;
            CHECK(msg->findInt32("port", &port));

            struct in_addr ifaceAddr;
            ifaceAddr.s_addr = INADDR_ANY;

            CHECK_EQ((status_t)OK,
                     mNetSession->createTCPDatagramSession(
                         ifaceAddr,
                         port,
                         notify,
                         &mServerSessionID));
            break;
        }

        case kWhatConnect:
        {
            sp<AMessage> notify = new AMessage(kWhatTimeSyncerNotify, id());
            mTimeSyncer = new TimeSyncer(mNetSession, notify);
            looper()->registerHandler(mTimeSyncer);
            mTimeSyncer->startServer(kTimeSyncerPort);

            AString host;
            CHECK(msg->findString("host", &host));

            int32_t port;
            CHECK(msg->findInt32("port", &port));

            notify = new AMessage(kWhatNetNotify, id());

            CHECK_EQ((status_t)OK,
                     mNetSession->createTCPDatagramSession(
                         0 /* localPort */,
                         host.c_str(),
                         port,
                         notify,
                         &mSessionID));
            break;
        }

        case kWhatNetNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatConnected:
                {
                    ALOGI("kWhatConnected");

                    (new AMessage(kWhatSendMore, id()))->post();
                    break;
                }

                case ANetworkSession::kWhatClientConnected:
                {
                    ALOGI("kWhatClientConnected");

                    CHECK_EQ(mSessionID, 0);
                    CHECK(msg->findInt32("sessionID", &mSessionID));

                    AString clientIP;
                    CHECK(msg->findString("client-ip", &clientIP));

                    mTimeSyncer->startClient(clientIP.c_str(), kTimeSyncerPort);
                    break;
                }

                case ANetworkSession::kWhatDatagram:
                {
                    sp<ABuffer> packet;
                    CHECK(msg->findBuffer("data", &packet));

                    CHECK_EQ(packet->size(), 12u);

                    int32_t counter = U32_AT(packet->data());
                    int64_t timeUs = U64_AT(packet->data() + 4);

                    if (mTimeOffsetValid) {
                        timeUs -= mTimeOffsetUs;
                        int64_t nowUs = ALooper::GetNowUs();
                        int64_t delayMs = (nowUs - timeUs) / 1000ll;

                        dumpDelay(counter, delayMs);
                    } else {
                        ALOGI("received %d", counter);
                    }
                    break;
                }

                case ANetworkSession::kWhatError:
                {
                    ALOGE("kWhatError");
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatTimeSyncerNotify:
        {
            CHECK(msg->findInt64("offset", &mTimeOffsetUs));
            mTimeOffsetValid = true;
            break;
        }

        case kWhatSendMore:
        {
            uint8_t buffer[4 + 8];
            buffer[0] = mCounter >> 24;
            buffer[1] = (mCounter >> 16) & 0xff;
            buffer[2] = (mCounter >> 8) & 0xff;
            buffer[3] = mCounter & 0xff;

            int64_t nowUs = ALooper::GetNowUs();

            buffer[4] = nowUs >> 56;
            buffer[5] = (nowUs >> 48) & 0xff;
            buffer[6] = (nowUs >> 40) & 0xff;
            buffer[7] = (nowUs >> 32) & 0xff;
            buffer[8] = (nowUs >> 24) & 0xff;
            buffer[9] = (nowUs >> 16) & 0xff;
            buffer[10] = (nowUs >> 8) & 0xff;
            buffer[11] = nowUs & 0xff;

            ++mCounter;

            CHECK_EQ((status_t)OK,
                     mNetSession->sendRequest(
                         mSessionID,
                         buffer,
                         sizeof(buffer),
                         true /* timeValid */,
                         nowUs));

            msg->post(100000ll);
            break;
        }

        case kWhatStop:
        {
            if (mSessionID != 0) {
                mNetSession->destroySession(mSessionID);
                mSessionID = 0;
            }

            if (mServerSessionID != 0) {
                mNetSession->destroySession(mServerSessionID);
                mServerSessionID = 0;
            }

            looper()->stop();
            break;
        }

        default:
            TRESPASS();
    }
}

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s -c host:port\tconnect to remote host\n"
            "               -l port   \tlisten\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    // srand(time(NULL));

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    int32_t connectToPort = -1;
    AString connectToHost;

    int32_t listenOnPort = -1;

    int res;
    while ((res = getopt(argc, argv, "hc:l:")) >= 0) {
        switch (res) {
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    usage(argv[0]);
                    exit(1);
                }

                connectToHost.setTo(optarg, colonPos - optarg);

                char *end;
                connectToPort = strtol(colonPos + 1, &end, 10);

                if (*end != '\0' || end == colonPos + 1
                        || connectToPort < 0 || connectToPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case 'l':
            {
                char *end;
                listenOnPort = strtol(optarg, &end, 10);

                if (*end != '\0' || end == optarg
                        || listenOnPort < 0 || listenOnPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case '?':
            case 'h':
                usage(argv[0]);
                exit(1);
        }
    }

    if ((listenOnPort < 0 && connectToPort < 0)
            || (listenOnPort >= 0 && connectToPort >= 0)) {
        fprintf(stderr,
                "You need to select either client or server mode.\n");
        exit(1);
    }

    sp<ANetworkSession> netSession = new ANetworkSession;
    netSession->start();

    sp<ALooper> looper = new ALooper;

    sp<TestHandler> handler = new TestHandler(netSession);
    looper->registerHandler(handler);

    if (listenOnPort) {
        handler->listen(listenOnPort);
    }

    if (connectToPort >= 0) {
        handler->connect(connectToHost.c_str(), connectToPort);
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}
