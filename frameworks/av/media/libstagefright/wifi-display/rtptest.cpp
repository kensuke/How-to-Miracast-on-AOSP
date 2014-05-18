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
#define LOG_TAG "rtptest"
#include <utils/Log.h>

#include "rtp/RTPSender.h"
#include "rtp/RTPReceiver.h"
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

#define MEDIA_FILENAME "/sdcard/Frame Counter HD 30FPS_1080p.mp4"

namespace android {

struct PacketSource : public RefBase {
    PacketSource() {}

    virtual sp<ABuffer> getNextAccessUnit() = 0;

protected:
    virtual ~PacketSource() {}

private:
    DISALLOW_EVIL_CONSTRUCTORS(PacketSource);
};

struct MediaPacketSource : public PacketSource {
    MediaPacketSource()
        : mMaxSampleSize(1024 * 1024) {
        mExtractor = new NuMediaExtractor;
        CHECK_EQ((status_t)OK,
                 mExtractor->setDataSource(MEDIA_FILENAME));

        bool haveVideo = false;
        for (size_t i = 0; i < mExtractor->countTracks(); ++i) {
            sp<AMessage> format;
            CHECK_EQ((status_t)OK, mExtractor->getTrackFormat(i, &format));

            AString mime;
            CHECK(format->findString("mime", &mime));

            if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime.c_str())) {
                mExtractor->selectTrack(i);
                haveVideo = true;
                break;
            }
        }

        CHECK(haveVideo);
    }

    virtual sp<ABuffer> getNextAccessUnit() {
        int64_t timeUs;
        status_t err = mExtractor->getSampleTime(&timeUs);

        if (err != OK) {
            return NULL;
        }

        sp<ABuffer> accessUnit = new ABuffer(mMaxSampleSize);
        CHECK_EQ((status_t)OK, mExtractor->readSampleData(accessUnit));

        accessUnit->meta()->setInt64("timeUs", timeUs);

        CHECK_EQ((status_t)OK, mExtractor->advance());

        return accessUnit;
    }

protected:
    virtual ~MediaPacketSource() {
    }

private:
    sp<NuMediaExtractor> mExtractor;
    size_t mMaxSampleSize;

    DISALLOW_EVIL_CONSTRUCTORS(MediaPacketSource);
};

struct SimplePacketSource : public PacketSource {
    SimplePacketSource()
        : mCounter(0) {
    }

    virtual sp<ABuffer> getNextAccessUnit() {
        sp<ABuffer> buffer = new ABuffer(4);
        uint8_t *dst = buffer->data();
        dst[0] = mCounter >> 24;
        dst[1] = (mCounter >> 16) & 0xff;
        dst[2] = (mCounter >> 8) & 0xff;
        dst[3] = mCounter & 0xff;

        buffer->meta()->setInt64("timeUs", mCounter * 1000000ll / kFrameRate);

        ++mCounter;

        return buffer;
    }

protected:
    virtual ~SimplePacketSource() {
    }

private:
    enum {
        kFrameRate = 30
    };

    uint32_t mCounter;

    DISALLOW_EVIL_CONSTRUCTORS(SimplePacketSource);
};

struct TestHandler : public AHandler {
    TestHandler(const sp<ANetworkSession> &netSession);

    void listen();
    void connect(const char *host, int32_t port);

protected:
    virtual ~TestHandler();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatListen,
        kWhatConnect,
        kWhatReceiverNotify,
        kWhatSenderNotify,
        kWhatSendMore,
        kWhatStop,
        kWhatTimeSyncerNotify,
    };

#if 1
    static const RTPBase::TransportMode kRTPMode = RTPBase::TRANSPORT_UDP;
    static const RTPBase::TransportMode kRTCPMode = RTPBase::TRANSPORT_UDP;
#else
    static const RTPBase::TransportMode kRTPMode = RTPBase::TRANSPORT_TCP;
    static const RTPBase::TransportMode kRTCPMode = RTPBase::TRANSPORT_NONE;
#endif

#if 1
    static const RTPBase::PacketizationMode kPacketizationMode
        = RTPBase::PACKETIZATION_H264;
#else
    static const RTPBase::PacketizationMode kPacketizationMode
        = RTPBase::PACKETIZATION_NONE;
#endif

    sp<ANetworkSession> mNetSession;
    sp<PacketSource> mSource;
    sp<RTPSender> mSender;
    sp<RTPReceiver> mReceiver;

    sp<TimeSyncer> mTimeSyncer;
    bool mTimeSyncerStarted;

    int64_t mFirstTimeRealUs;
    int64_t mFirstTimeMediaUs;

    int64_t mTimeOffsetUs;
    bool mTimeOffsetValid;

    status_t readMore();

    DISALLOW_EVIL_CONSTRUCTORS(TestHandler);
};

TestHandler::TestHandler(const sp<ANetworkSession> &netSession)
    : mNetSession(netSession),
      mTimeSyncerStarted(false),
      mFirstTimeRealUs(-1ll),
      mFirstTimeMediaUs(-1ll),
      mTimeOffsetUs(-1ll),
      mTimeOffsetValid(false) {
}

TestHandler::~TestHandler() {
}

void TestHandler::listen() {
    sp<AMessage> msg = new AMessage(kWhatListen, id());
    msg->post();
}

void TestHandler::connect(const char *host, int32_t port) {
    sp<AMessage> msg = new AMessage(kWhatConnect, id());
    msg->setString("host", host);
    msg->setInt32("port", port);
    msg->post();
}

static void dumpDelay(int64_t delayMs) {
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

    ALOGI("(%4lld ms) %s\n",
          delayMs,
          kPattern + kPatternSize - n);
}

void TestHandler::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatListen:
        {
            sp<AMessage> notify = new AMessage(kWhatTimeSyncerNotify, id());
            mTimeSyncer = new TimeSyncer(mNetSession, notify);
            looper()->registerHandler(mTimeSyncer);

            notify = new AMessage(kWhatReceiverNotify, id());
            mReceiver = new RTPReceiver(
                    mNetSession, notify, RTPReceiver::FLAG_AUTO_CONNECT);
            looper()->registerHandler(mReceiver);

            CHECK_EQ((status_t)OK,
                     mReceiver->registerPacketType(33, kPacketizationMode));

            int32_t receiverRTPPort;
            CHECK_EQ((status_t)OK,
                     mReceiver->initAsync(
                         kRTPMode,
                         kRTCPMode,
                         &receiverRTPPort));

            printf("picked receiverRTPPort %d\n", receiverRTPPort);

#if 0
            CHECK_EQ((status_t)OK,
                     mReceiver->connect(
                         "127.0.0.1", senderRTPPort, senderRTPPort + 1));
#endif
            break;
        }

        case kWhatConnect:
        {
            AString host;
            CHECK(msg->findString("host", &host));

            sp<AMessage> notify = new AMessage(kWhatTimeSyncerNotify, id());
            mTimeSyncer = new TimeSyncer(mNetSession, notify);
            looper()->registerHandler(mTimeSyncer);
            mTimeSyncer->startServer(8123);

            int32_t receiverRTPPort;
            CHECK(msg->findInt32("port", &receiverRTPPort));

#if 1
            mSource = new MediaPacketSource;
#else
            mSource = new SimplePacketSource;
#endif

            notify = new AMessage(kWhatSenderNotify, id());
            mSender = new RTPSender(mNetSession, notify);

            looper()->registerHandler(mSender);

            int32_t senderRTPPort;
            CHECK_EQ((status_t)OK,
                     mSender->initAsync(
                         host.c_str(),
                         receiverRTPPort,
                         kRTPMode,
                         kRTCPMode == RTPBase::TRANSPORT_NONE
                            ? -1 : receiverRTPPort + 1,
                         kRTCPMode,
                         &senderRTPPort));

            printf("picked senderRTPPort %d\n", senderRTPPort);
            break;
        }

        case kWhatSenderNotify:
        {
            ALOGI("kWhatSenderNotify");

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case RTPSender::kWhatInitDone:
                {
                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGI("RTPSender::initAsync completed w/ err %d", err);

                    if (err == OK) {
                        err = readMore();

                        if (err != OK) {
                            (new AMessage(kWhatStop, id()))->post();
                        }
                    }
                    break;
                }

                case RTPSender::kWhatError:
                    break;
            }
            break;
        }

        case kWhatReceiverNotify:
        {
            ALOGV("kWhatReceiverNotify");

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case RTPReceiver::kWhatInitDone:
                {
                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGI("RTPReceiver::initAsync completed w/ err %d", err);
                    break;
                }

                case RTPReceiver::kWhatError:
                    break;

                case RTPReceiver::kWhatAccessUnit:
                {
#if 0
                    if (!mTimeSyncerStarted) {
                        mTimeSyncer->startClient("172.18.41.216", 8123);
                        mTimeSyncerStarted = true;
                    }

                    sp<ABuffer> accessUnit;
                    CHECK(msg->findBuffer("accessUnit", &accessUnit));

                    int64_t timeUs;
                    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

                    if (mTimeOffsetValid) {
                        timeUs -= mTimeOffsetUs;
                        int64_t nowUs = ALooper::GetNowUs();
                        int64_t delayMs = (nowUs - timeUs) / 1000ll;

                        dumpDelay(delayMs);
                    }
#endif
                    break;
                }

                case RTPReceiver::kWhatPacketLost:
                    ALOGV("kWhatPacketLost");
                    break;

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatSendMore:
        {
            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

            CHECK_EQ((status_t)OK,
                     mSender->queueBuffer(
                         accessUnit,
                         33,
                         kPacketizationMode));

            status_t err = readMore();

            if (err != OK) {
                (new AMessage(kWhatStop, id()))->post();
            }
            break;
        }

        case kWhatStop:
        {
            if (mReceiver != NULL) {
                looper()->unregisterHandler(mReceiver->id());
                mReceiver.clear();
            }

            if (mSender != NULL) {
                looper()->unregisterHandler(mSender->id());
                mSender.clear();
            }

            mSource.clear();

            looper()->stop();
            break;
        }

        case kWhatTimeSyncerNotify:
        {
            CHECK(msg->findInt64("offset", &mTimeOffsetUs));
            mTimeOffsetValid = true;
            break;
        }

        default:
            TRESPASS();
    }
}

status_t TestHandler::readMore() {
    sp<ABuffer> accessUnit = mSource->getNextAccessUnit();

    if (accessUnit == NULL) {
        return ERROR_END_OF_STREAM;
    }

    int64_t timeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

    int64_t nowUs = ALooper::GetNowUs();
    int64_t whenUs;

    if (mFirstTimeRealUs < 0ll) {
        mFirstTimeRealUs = whenUs = nowUs;
        mFirstTimeMediaUs = timeUs;
    } else {
        whenUs = mFirstTimeRealUs + timeUs - mFirstTimeMediaUs;
    }

    accessUnit->meta()->setInt64("timeUs", whenUs);

    sp<AMessage> msg = new AMessage(kWhatSendMore, id());
    msg->setBuffer("accessUnit", accessUnit);
    msg->post(whenUs - nowUs);

    return OK;
}

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s -c host:port\tconnect to remote host\n"
            "               -l       \tlisten\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    // srand(time(NULL));

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    bool listen = false;
    int32_t connectToPort = -1;
    AString connectToHost;

    int res;
    while ((res = getopt(argc, argv, "hc:l")) >= 0) {
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
                        || connectToPort < 1 || connectToPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case 'l':
            {
                listen = true;
                break;
            }

            case '?':
            case 'h':
                usage(argv[0]);
                exit(1);
        }
    }

    if (!listen && connectToPort < 0) {
        fprintf(stderr,
                "You need to select either client or server mode.\n");
        exit(1);
    }

    sp<ANetworkSession> netSession = new ANetworkSession;
    netSession->start();

    sp<ALooper> looper = new ALooper;

    sp<TestHandler> handler = new TestHandler(netSession);
    looper->registerHandler(handler);

    if (listen) {
        handler->listen();
    }

    if (connectToPort >= 0) {
        handler->connect(connectToHost.c_str(), connectToPort);
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}

