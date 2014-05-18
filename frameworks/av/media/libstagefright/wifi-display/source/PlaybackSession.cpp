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

//#define LOG_NDEBUG 0
#define LOG_TAG "PlaybackSession"
#include <utils/Log.h>

#include "PlaybackSession.h"

#include "Converter.h"
#include "MediaPuller.h"
#include "RepeaterSource.h"
#include "include/avc_utils.h"
#include "WifiDisplaySource.h"

#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <media/IHDCP.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/stagefright/SurfaceMediaSource.h>
#include <media/stagefright/Utils.h>

#include <OMX_IVCommon.h>

namespace android {

struct WifiDisplaySource::PlaybackSession::Track : public AHandler {
    enum {
        kWhatStopped,
    };

    Track(const sp<AMessage> &notify,
          const sp<ALooper> &pullLooper,
          const sp<ALooper> &codecLooper,
          const sp<MediaPuller> &mediaPuller,
          const sp<Converter> &converter);

    Track(const sp<AMessage> &notify, const sp<AMessage> &format);

    void setRepeaterSource(const sp<RepeaterSource> &source);

    sp<AMessage> getFormat();
    bool isAudio() const;

    const sp<Converter> &converter() const;
    const sp<RepeaterSource> &repeaterSource() const;

    ssize_t mediaSenderTrackIndex() const;
    void setMediaSenderTrackIndex(size_t index);

    status_t start();
    void stopAsync();

    void pause();
    void resume();

    void queueAccessUnit(const sp<ABuffer> &accessUnit);
    sp<ABuffer> dequeueAccessUnit();

    bool hasOutputBuffer(int64_t *timeUs) const;
    void queueOutputBuffer(const sp<ABuffer> &accessUnit);
    sp<ABuffer> dequeueOutputBuffer();

#if SUSPEND_VIDEO_IF_IDLE
    bool isSuspended() const;
#endif

    size_t countQueuedOutputBuffers() const {
        return mQueuedOutputBuffers.size();
    }

    void requestIDRFrame();

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~Track();

private:
    enum {
        kWhatMediaPullerStopped,
    };

    sp<AMessage> mNotify;
    sp<ALooper> mPullLooper;
    sp<ALooper> mCodecLooper;
    sp<MediaPuller> mMediaPuller;
    sp<Converter> mConverter;
    sp<AMessage> mFormat;
    bool mStarted;
    ssize_t mMediaSenderTrackIndex;
    bool mIsAudio;
    List<sp<ABuffer> > mQueuedAccessUnits;
    sp<RepeaterSource> mRepeaterSource;
    List<sp<ABuffer> > mQueuedOutputBuffers;
    int64_t mLastOutputBufferQueuedTimeUs;

    static bool IsAudioFormat(const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

WifiDisplaySource::PlaybackSession::Track::Track(
        const sp<AMessage> &notify,
        const sp<ALooper> &pullLooper,
        const sp<ALooper> &codecLooper,
        const sp<MediaPuller> &mediaPuller,
        const sp<Converter> &converter)
    : mNotify(notify),
      mPullLooper(pullLooper),
      mCodecLooper(codecLooper),
      mMediaPuller(mediaPuller),
      mConverter(converter),
      mStarted(false),
      mIsAudio(IsAudioFormat(mConverter->getOutputFormat())),
      mLastOutputBufferQueuedTimeUs(-1ll) {
}

WifiDisplaySource::PlaybackSession::Track::Track(
        const sp<AMessage> &notify, const sp<AMessage> &format)
    : mNotify(notify),
      mFormat(format),
      mStarted(false),
      mIsAudio(IsAudioFormat(format)),
      mLastOutputBufferQueuedTimeUs(-1ll) {
}

WifiDisplaySource::PlaybackSession::Track::~Track() {
    CHECK(!mStarted);
}

// static
bool WifiDisplaySource::PlaybackSession::Track::IsAudioFormat(
        const sp<AMessage> &format) {
    AString mime;
    CHECK(format->findString("mime", &mime));

    return !strncasecmp(mime.c_str(), "audio/", 6);
}

sp<AMessage> WifiDisplaySource::PlaybackSession::Track::getFormat() {
    return mFormat != NULL ? mFormat : mConverter->getOutputFormat();
}

bool WifiDisplaySource::PlaybackSession::Track::isAudio() const {
    return mIsAudio;
}

const sp<Converter> &WifiDisplaySource::PlaybackSession::Track::converter() const {
    return mConverter;
}

const sp<RepeaterSource> &
WifiDisplaySource::PlaybackSession::Track::repeaterSource() const {
    return mRepeaterSource;
}

ssize_t WifiDisplaySource::PlaybackSession::Track::mediaSenderTrackIndex() const {
    CHECK_GE(mMediaSenderTrackIndex, 0);
    return mMediaSenderTrackIndex;
}

void WifiDisplaySource::PlaybackSession::Track::setMediaSenderTrackIndex(
        size_t index) {
    mMediaSenderTrackIndex = index;
}

status_t WifiDisplaySource::PlaybackSession::Track::start() {
    ALOGV("Track::start isAudio=%d", mIsAudio);

    CHECK(!mStarted);

    status_t err = OK;

    if (mMediaPuller != NULL) {
        err = mMediaPuller->start();
    }

    if (err == OK) {
        mStarted = true;
    }

    return err;
}

void WifiDisplaySource::PlaybackSession::Track::stopAsync() {
    ALOGV("Track::stopAsync isAudio=%d", mIsAudio);

    if (mConverter != NULL) {
        mConverter->shutdownAsync();
    }

    sp<AMessage> msg = new AMessage(kWhatMediaPullerStopped, id());

    if (mStarted && mMediaPuller != NULL) {
        if (mRepeaterSource != NULL) {
            // Let's unblock MediaPuller's MediaSource::read().
            mRepeaterSource->wakeUp();
        }

        mMediaPuller->stopAsync(msg);
    } else {
        mStarted = false;
        msg->post();
    }
}

void WifiDisplaySource::PlaybackSession::Track::pause() {
    mMediaPuller->pause();
}

void WifiDisplaySource::PlaybackSession::Track::resume() {
    mMediaPuller->resume();
}

void WifiDisplaySource::PlaybackSession::Track::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatMediaPullerStopped:
        {
            mConverter.clear();

            mStarted = false;

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStopped);
            notify->post();

            ALOGI("kWhatStopped %s posted", mIsAudio ? "audio" : "video");
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySource::PlaybackSession::Track::queueAccessUnit(
        const sp<ABuffer> &accessUnit) {
    mQueuedAccessUnits.push_back(accessUnit);
}

sp<ABuffer> WifiDisplaySource::PlaybackSession::Track::dequeueAccessUnit() {
    if (mQueuedAccessUnits.empty()) {
        return NULL;
    }

    sp<ABuffer> accessUnit = *mQueuedAccessUnits.begin();
    CHECK(accessUnit != NULL);

    mQueuedAccessUnits.erase(mQueuedAccessUnits.begin());

    return accessUnit;
}

void WifiDisplaySource::PlaybackSession::Track::setRepeaterSource(
        const sp<RepeaterSource> &source) {
    mRepeaterSource = source;
}

void WifiDisplaySource::PlaybackSession::Track::requestIDRFrame() {
    if (mIsAudio) {
        return;
    }

    if (mRepeaterSource != NULL) {
        mRepeaterSource->wakeUp();
    }

    mConverter->requestIDRFrame();
}

bool WifiDisplaySource::PlaybackSession::Track::hasOutputBuffer(
        int64_t *timeUs) const {
    *timeUs = 0ll;

    if (mQueuedOutputBuffers.empty()) {
        return false;
    }

    const sp<ABuffer> &outputBuffer = *mQueuedOutputBuffers.begin();

    CHECK(outputBuffer->meta()->findInt64("timeUs", timeUs));

    return true;
}

void WifiDisplaySource::PlaybackSession::Track::queueOutputBuffer(
        const sp<ABuffer> &accessUnit) {
    mQueuedOutputBuffers.push_back(accessUnit);
    mLastOutputBufferQueuedTimeUs = ALooper::GetNowUs();
}

sp<ABuffer> WifiDisplaySource::PlaybackSession::Track::dequeueOutputBuffer() {
    CHECK(!mQueuedOutputBuffers.empty());

    sp<ABuffer> outputBuffer = *mQueuedOutputBuffers.begin();
    mQueuedOutputBuffers.erase(mQueuedOutputBuffers.begin());

    return outputBuffer;
}

#if SUSPEND_VIDEO_IF_IDLE
bool WifiDisplaySource::PlaybackSession::Track::isSuspended() const {
    if (!mQueuedOutputBuffers.empty()) {
        return false;
    }

    if (mLastOutputBufferQueuedTimeUs < 0ll) {
        // We've never seen an output buffer queued, but tracks start
        // out live, not suspended.
        return false;
    }

    // If we've not seen new output data for 60ms or more, we consider
    // this track suspended for the time being.
    return (ALooper::GetNowUs() - mLastOutputBufferQueuedTimeUs) > 60000ll;
}
#endif

////////////////////////////////////////////////////////////////////////////////

WifiDisplaySource::PlaybackSession::PlaybackSession(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        const in_addr &interfaceAddr,
        const sp<IHDCP> &hdcp,
        const char *path)
    : mNetSession(netSession),
      mNotify(notify),
      mInterfaceAddr(interfaceAddr),
      mHDCP(hdcp),
      mLocalRTPPort(-1),
      mWeAreDead(false),
      mPaused(false),
      mLastLifesignUs(),
      mVideoTrackIndex(-1),
      mPrevTimeUs(-1ll),
      mPullExtractorPending(false),
      mPullExtractorGeneration(0),
      mFirstSampleTimeRealUs(-1ll),
      mFirstSampleTimeUs(-1ll) {
    if (path != NULL) {
        mMediaPath.setTo(path);
    }

    // Source settings
    mWidth = -1;
    mHeight = -1;
    mFramerate = -1;

    // self parse
    FILE* fp = fopen("/data/data/com.example.mira4u/shared_prefs/prefs.xml", "r");
    if (fp == NULL) {
        ALOGE("PlaybackSession() fopen error[%d] (%4d, %4d)[%d]Hz", errno, mWidth, mHeight, mFramerate);
        return;
    }

    char line[80];
    while( fgets(line , sizeof(line) , fp) != NULL ) {
        char lin[80];
        memset(lin, 0, 80);
        ALOGD("[%s]", strncpy(lin, line, strlen(line)-1)); // delete CR
        int32_t val1 = -1, val2 = -1;
        int32_t ret = sscanf(line, "    <string name=\"persist.sys.wfd.resolution\">%dx%d</string>", &val1, &val2);
        if (ret == 2 && val1 > 0 && val2 > 0) {
            mWidth = val1;
            mHeight = val2;
        }
        val1 = -1;
        ret = sscanf(line, "    <string name=\"persist.sys.wfd.framerate\">%d</string>", &val1);
        if (ret == 1 && val1 > 0) {
            mFramerate = val1;
        }
    }
    fclose(fp);
    ALOGD("PlaybackSession() (%4d, %4d)[%d]Hz", mWidth, mHeight, mFramerate);
}

status_t WifiDisplaySource::PlaybackSession::init(
        const char *clientIP,
        int32_t clientRtp,
        RTPSender::TransportMode rtpMode,
        int32_t clientRtcp,
        RTPSender::TransportMode rtcpMode,
        bool enableAudio,
        bool usePCMAudio,
        bool enableVideo,
        VideoFormats::ResolutionType videoResolutionType,
        size_t videoResolutionIndex,
        VideoFormats::ProfileType videoProfileType,
        VideoFormats::LevelType videoLevelType) {
    sp<AMessage> notify = new AMessage(kWhatMediaSenderNotify, id());
    mMediaSender = new MediaSender(mNetSession, notify);
    looper()->registerHandler(mMediaSender);

    mMediaSender->setHDCP(mHDCP);

    status_t err = setupPacketizer(
            enableAudio,
            usePCMAudio,
            enableVideo,
            videoResolutionType,
            videoResolutionIndex,
            videoProfileType,
            videoLevelType);

    if (err == OK) {
        err = mMediaSender->initAsync(
                -1 /* trackIndex */,
                clientIP,
                clientRtp,
                rtpMode,
                clientRtcp,
                rtcpMode,
                &mLocalRTPPort);
    }

    if (err != OK) {
        mLocalRTPPort = -1;

        looper()->unregisterHandler(mMediaSender->id());
        mMediaSender.clear();

        return err;
    }

    updateLiveness();

    return OK;
}

WifiDisplaySource::PlaybackSession::~PlaybackSession() {
}

int32_t WifiDisplaySource::PlaybackSession::getRTPPort() const {
    return mLocalRTPPort;
}

int64_t WifiDisplaySource::PlaybackSession::getLastLifesignUs() const {
    return mLastLifesignUs;
}

void WifiDisplaySource::PlaybackSession::updateLiveness() {
    mLastLifesignUs = ALooper::GetNowUs();
}

status_t WifiDisplaySource::PlaybackSession::play() {
    updateLiveness();

    (new AMessage(kWhatResume, id()))->post();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::onMediaSenderInitialized() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        CHECK_EQ((status_t)OK, mTracks.editValueAt(i)->start());
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionEstablished);
    notify->post();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::pause() {
    updateLiveness();

    (new AMessage(kWhatPause, id()))->post();

    return OK;
}

void WifiDisplaySource::PlaybackSession::destroyAsync() {
    ALOGI("destroyAsync");

    for (size_t i = 0; i < mTracks.size(); ++i) {
        mTracks.valueAt(i)->stopAsync();
    }
}

void WifiDisplaySource::PlaybackSession::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatConverterNotify:
        {
            if (mWeAreDead) {
                ALOGV("dropping msg '%s' because we're dead",
                      msg->debugString().c_str());

                break;
            }

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Converter::kWhatAccessUnit) {
                sp<ABuffer> accessUnit;
                CHECK(msg->findBuffer("accessUnit", &accessUnit));

                const sp<Track> &track = mTracks.valueFor(trackIndex);

                status_t err = mMediaSender->queueAccessUnit(
                        track->mediaSenderTrackIndex(),
                        accessUnit);

                if (err != OK) {
                    notifySessionDead();
                }
                break;
            } else if (what == Converter::kWhatEOS) {
                CHECK_EQ(what, Converter::kWhatEOS);

                ALOGI("output EOS on track %d", trackIndex);

                ssize_t index = mTracks.indexOfKey(trackIndex);
                CHECK_GE(index, 0);

                const sp<Converter> &converter =
                    mTracks.valueAt(index)->converter();
                looper()->unregisterHandler(converter->id());

                mTracks.removeItemsAt(index);

                if (mTracks.isEmpty()) {
                    ALOGI("Reached EOS");
                }
            } else if (what != Converter::kWhatShutdownCompleted) {
                CHECK_EQ(what, Converter::kWhatError);

                status_t err;
                CHECK(msg->findInt32("err", &err));

                ALOGE("converter signaled error %d", err);

                notifySessionDead();
            }
            break;
        }

        case kWhatMediaSenderNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == MediaSender::kWhatInitDone) {
                status_t err;
                CHECK(msg->findInt32("err", &err));

                if (err == OK) {
                    onMediaSenderInitialized();
                } else {
                    notifySessionDead();
                }
            } else if (what == MediaSender::kWhatError) {
                notifySessionDead();
            } else if (what == MediaSender::kWhatNetworkStall) {
                size_t numBytesQueued;
                CHECK(msg->findSize("numBytesQueued", &numBytesQueued));

                if (mVideoTrackIndex >= 0) {
                    const sp<Track> &videoTrack =
                        mTracks.valueFor(mVideoTrackIndex);

                    sp<Converter> converter = videoTrack->converter();
                    if (converter != NULL) {
                        converter->dropAFrame();
                    }
                }
            } else if (what == MediaSender::kWhatInformSender) {
                onSinkFeedback(msg);
            } else {
                TRESPASS();
            }
            break;
        }

        case kWhatTrackNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Track::kWhatStopped) {
                ALOGI("Track %d stopped", trackIndex);

                sp<Track> track = mTracks.valueFor(trackIndex);
                looper()->unregisterHandler(track->id());
                mTracks.removeItem(trackIndex);
                track.clear();

                if (!mTracks.isEmpty()) {
                    ALOGI("not all tracks are stopped yet");
                    break;
                }

                looper()->unregisterHandler(mMediaSender->id());
                mMediaSender.clear();

                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("what", kWhatSessionDestroyed);
                notify->post();
            }
            break;
        }

        case kWhatPause:
        {
            if (mExtractor != NULL) {
                ++mPullExtractorGeneration;
                mFirstSampleTimeRealUs = -1ll;
                mFirstSampleTimeUs = -1ll;
            }

            if (mPaused) {
                break;
            }

            for (size_t i = 0; i < mTracks.size(); ++i) {
                mTracks.editValueAt(i)->pause();
            }

            mPaused = true;
            break;
        }

        case kWhatResume:
        {
            if (mExtractor != NULL) {
                schedulePullExtractor();
            }

            if (!mPaused) {
                break;
            }

            for (size_t i = 0; i < mTracks.size(); ++i) {
                mTracks.editValueAt(i)->resume();
            }

            mPaused = false;
            break;
        }

        case kWhatPullExtractorSample:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mPullExtractorGeneration) {
                break;
            }

            mPullExtractorPending = false;

            onPullExtractor();
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySource::PlaybackSession::onSinkFeedback(const sp<AMessage> &msg) {
    int64_t avgLatencyUs;
    CHECK(msg->findInt64("avgLatencyUs", &avgLatencyUs));

    int64_t maxLatencyUs;
    CHECK(msg->findInt64("maxLatencyUs", &maxLatencyUs));

    ALOGI("sink reports avg. latency of %lld ms (max %lld ms)",
          avgLatencyUs / 1000ll,
          maxLatencyUs / 1000ll);

    if (mVideoTrackIndex >= 0) {
        const sp<Track> &videoTrack = mTracks.valueFor(mVideoTrackIndex);
        sp<Converter> converter = videoTrack->converter();

        if (converter != NULL) {
            int32_t videoBitrate =
                Converter::GetInt32Property("media.wfd.video-bitrate", -1);

            char val[PROPERTY_VALUE_MAX];
            if (videoBitrate < 0
                    && property_get("media.wfd.video-bitrate", val, NULL)
                    && !strcasecmp("adaptive", val)) {
                videoBitrate = converter->getVideoBitrate();

                if (avgLatencyUs > 300000ll) {
                    videoBitrate *= 0.6;
                } else if (avgLatencyUs < 100000ll) {
                    videoBitrate *= 1.1;
                }
            }

            if (videoBitrate > 0) {
                if (videoBitrate < 500000) {
                    videoBitrate = 500000;
                } else if (videoBitrate > 10000000) {
                    videoBitrate = 10000000;
                }

                if (videoBitrate != converter->getVideoBitrate()) {
                    ALOGI("setting video bitrate to %d bps", videoBitrate);

                    converter->setVideoBitrate(videoBitrate);
                }
            }
        }

        sp<RepeaterSource> repeaterSource = videoTrack->repeaterSource();
        if (repeaterSource != NULL) {
            double rateHz =
                Converter::GetInt32Property(
                        "media.wfd.video-framerate", -1);

            char val[PROPERTY_VALUE_MAX];
            if (rateHz < 0.0
                    && property_get("media.wfd.video-framerate", val, NULL)
                    && !strcasecmp("adaptive", val)) {
                 rateHz = repeaterSource->getFrameRate();

                if (avgLatencyUs > 300000ll) {
                    rateHz *= 0.9;
                } else if (avgLatencyUs < 200000ll) {
                    rateHz *= 1.1;
                }
            }

            if (rateHz > 0) {
                if (rateHz < 5.0) {
                    rateHz = 5.0;
                } else if (rateHz > 30.0) {
                    rateHz = 30.0;
                }

                if (rateHz != repeaterSource->getFrameRate()) {
                    ALOGI("setting frame rate to %.2f Hz", rateHz);

                    repeaterSource->setFrameRate(rateHz);
                }
            }
        }
    }
}

status_t WifiDisplaySource::PlaybackSession::setupMediaPacketizer(
        bool enableAudio, bool enableVideo) {
    DataSource::RegisterDefaultSniffers();

    mExtractor = new NuMediaExtractor;

    status_t err = mExtractor->setDataSource(mMediaPath.c_str());

    if (err != OK) {
        return err;
    }

    size_t n = mExtractor->countTracks();
    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < n; ++i) {
        sp<AMessage> format;
        err = mExtractor->getTrackFormat(i, &format);

        if (err != OK) {
            continue;
        }

        AString mime;
        CHECK(format->findString("mime", &mime));

        bool isAudio = !strncasecmp(mime.c_str(), "audio/", 6);
        bool isVideo = !strncasecmp(mime.c_str(), "video/", 6);

        if (isAudio && enableAudio && !haveAudio) {
            haveAudio = true;
        } else if (isVideo && enableVideo && !haveVideo) {
            haveVideo = true;
        } else {
            continue;
        }

        err = mExtractor->selectTrack(i);

        size_t trackIndex = mTracks.size();

        sp<AMessage> notify = new AMessage(kWhatTrackNotify, id());
        notify->setSize("trackIndex", trackIndex);

        sp<Track> track = new Track(notify, format);
        looper()->registerHandler(track);

        mTracks.add(trackIndex, track);

        mExtractorTrackToInternalTrack.add(i, trackIndex);

        if (isVideo) {
            mVideoTrackIndex = trackIndex;
        }

        uint32_t flags = MediaSender::FLAG_MANUALLY_PREPEND_SPS_PPS;

        ssize_t mediaSenderTrackIndex =
            mMediaSender->addTrack(format, flags);
        CHECK_GE(mediaSenderTrackIndex, 0);

        track->setMediaSenderTrackIndex(mediaSenderTrackIndex);

        if ((haveAudio || !enableAudio) && (haveVideo || !enableVideo)) {
            break;
        }
    }

    return OK;
}

void WifiDisplaySource::PlaybackSession::schedulePullExtractor() {
    if (mPullExtractorPending) {
        return;
    }

    int64_t sampleTimeUs;
    status_t err = mExtractor->getSampleTime(&sampleTimeUs);

    int64_t nowUs = ALooper::GetNowUs();

    if (mFirstSampleTimeRealUs < 0ll) {
        mFirstSampleTimeRealUs = nowUs;
        mFirstSampleTimeUs = sampleTimeUs;
    }

    int64_t whenUs = sampleTimeUs - mFirstSampleTimeUs + mFirstSampleTimeRealUs;

    sp<AMessage> msg = new AMessage(kWhatPullExtractorSample, id());
    msg->setInt32("generation", mPullExtractorGeneration);
    msg->post(whenUs - nowUs);

    mPullExtractorPending = true;
}

void WifiDisplaySource::PlaybackSession::onPullExtractor() {
    sp<ABuffer> accessUnit = new ABuffer(1024 * 1024);
    status_t err = mExtractor->readSampleData(accessUnit);
    if (err != OK) {
        // EOS.
        return;
    }

    int64_t timeUs;
    CHECK_EQ((status_t)OK, mExtractor->getSampleTime(&timeUs));

    accessUnit->meta()->setInt64(
            "timeUs", mFirstSampleTimeRealUs + timeUs - mFirstSampleTimeUs);

    size_t trackIndex;
    CHECK_EQ((status_t)OK, mExtractor->getSampleTrackIndex(&trackIndex));

    sp<AMessage> msg = new AMessage(kWhatConverterNotify, id());

    msg->setSize(
            "trackIndex", mExtractorTrackToInternalTrack.valueFor(trackIndex));

    msg->setInt32("what", Converter::kWhatAccessUnit);
    msg->setBuffer("accessUnit", accessUnit);
    msg->post();

    mExtractor->advance();

    schedulePullExtractor();
}

status_t WifiDisplaySource::PlaybackSession::setupPacketizer(
        bool enableAudio,
        bool usePCMAudio,
        bool enableVideo,
        VideoFormats::ResolutionType videoResolutionType,
        size_t videoResolutionIndex,
        VideoFormats::ProfileType videoProfileType,
        VideoFormats::LevelType videoLevelType) {
    CHECK(enableAudio || enableVideo);

    if (!mMediaPath.empty()) {
        return setupMediaPacketizer(enableAudio, enableVideo);
    }

    if (enableVideo) {
        status_t err = addVideoSource(
                videoResolutionType, videoResolutionIndex, videoProfileType,
                videoLevelType);

        if (err != OK) {
            return err;
        }
    }

    if (!enableAudio) {
        return OK;
    }

    return addAudioSource(usePCMAudio);
}

status_t WifiDisplaySource::PlaybackSession::addSource(
        bool isVideo, const sp<MediaSource> &source, bool isRepeaterSource,
        bool usePCMAudio, unsigned profileIdc, unsigned levelIdc,
        unsigned constraintSet, size_t *numInputBuffers) {
    CHECK(!usePCMAudio || !isVideo);
    CHECK(!isRepeaterSource || isVideo);
    CHECK(!profileIdc || isVideo);
    CHECK(!levelIdc || isVideo);
    CHECK(!constraintSet || isVideo);

    sp<ALooper> pullLooper = new ALooper;
    pullLooper->setName("pull_looper");

    pullLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    sp<ALooper> codecLooper = new ALooper;
    codecLooper->setName("codec_looper");

    codecLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    size_t trackIndex;

    sp<AMessage> notify;

    trackIndex = mTracks.size();

    sp<AMessage> format;
    status_t err = convertMetaDataToMessage(source->getFormat(), &format);
    CHECK_EQ(err, (status_t)OK);

    if (isVideo) {
        format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
        format->setInt32("store-metadata-in-buffers", true);
        format->setInt32("store-metadata-in-buffers-output", (mHDCP != NULL)
                && (mHDCP->getCaps() & HDCPModule::HDCP_CAPS_ENCRYPT_NATIVE));
        format->setInt32(
                "color-format", OMX_COLOR_FormatAndroidOpaque);
        format->setInt32("profile-idc", profileIdc);
        format->setInt32("level-idc", levelIdc);
        format->setInt32("constraint-set", constraintSet);
    } else {
        format->setString(
                "mime",
                usePCMAudio
                    ? MEDIA_MIMETYPE_AUDIO_RAW : MEDIA_MIMETYPE_AUDIO_AAC);
    }

    notify = new AMessage(kWhatConverterNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Converter> converter = new Converter(notify, codecLooper, format);

    looper()->registerHandler(converter);

    err = converter->init();
    if (err != OK) {
        ALOGE("%s converter returned err %d", isVideo ? "video" : "audio", err);

        looper()->unregisterHandler(converter->id());
        return err;
    }

    notify = new AMessage(Converter::kWhatMediaPullerNotify, converter->id());
    notify->setSize("trackIndex", trackIndex);

    sp<MediaPuller> puller = new MediaPuller(source, notify);
    pullLooper->registerHandler(puller);

    if (numInputBuffers != NULL) {
        *numInputBuffers = converter->getInputBufferCount();
    }

    notify = new AMessage(kWhatTrackNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Track> track = new Track(
            notify, pullLooper, codecLooper, puller, converter);

    if (isRepeaterSource) {
        track->setRepeaterSource(static_cast<RepeaterSource *>(source.get()));
    }

    looper()->registerHandler(track);

    mTracks.add(trackIndex, track);

    if (isVideo) {
        mVideoTrackIndex = trackIndex;
    }

    uint32_t flags = 0;
    if (converter->needToManuallyPrependSPSPPS()) {
        flags |= MediaSender::FLAG_MANUALLY_PREPEND_SPS_PPS;
    }

    ssize_t mediaSenderTrackIndex =
        mMediaSender->addTrack(converter->getOutputFormat(), flags);
    CHECK_GE(mediaSenderTrackIndex, 0);

    track->setMediaSenderTrackIndex(mediaSenderTrackIndex);

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addVideoSource(
        VideoFormats::ResolutionType videoResolutionType,
        size_t videoResolutionIndex,
        VideoFormats::ProfileType videoProfileType,
        VideoFormats::LevelType videoLevelType) {
    size_t width, height, framesPerSecond;
    bool interlaced;
    CHECK(VideoFormats::GetConfiguration(
                videoResolutionType,
                videoResolutionIndex,
                &width,
                &height,
                &framesPerSecond,
                &interlaced));

    unsigned profileIdc, levelIdc, constraintSet;
    CHECK(VideoFormats::GetProfileLevel(
                videoProfileType,
                videoLevelType,
                &profileIdc,
                &levelIdc,
                &constraintSet));

    ALOGI("addVideoSource() default  params (%4d, %4d)(%d)Hz", width, height, framesPerSecond);
    if (mWidth > 0 && mHeight > 0) {
        width = mWidth;
        height = mHeight;
    }
    framesPerSecond = mFramerate > 0 ? mFramerate : framesPerSecond;
    ALOGI("addVideoSource() changed? params (%4d, %4d)(%d)Hz", width, height, framesPerSecond);

    sp<SurfaceMediaSource> source = new SurfaceMediaSource(width, height);

    source->setUseAbsoluteTimestamps();

    sp<RepeaterSource> videoSource =
        new RepeaterSource(source, framesPerSecond);

    size_t numInputBuffers;
    status_t err = addSource(
            true /* isVideo */, videoSource, true /* isRepeaterSource */,
            false /* usePCMAudio */, profileIdc, levelIdc, constraintSet,
            &numInputBuffers);

    if (err != OK) {
        return err;
    }

    err = source->setMaxAcquiredBufferCount(numInputBuffers);
    CHECK_EQ(err, (status_t)OK);

    mBufferQueue = source->getBufferQueue();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addAudioSource(bool usePCMAudio) {
    sp<AudioSource> audioSource = new AudioSource(
            AUDIO_SOURCE_REMOTE_SUBMIX,
            48000 /* sampleRate */,
            2 /* channelCount */);

    if (audioSource->initCheck() == OK) {
        return addSource(
                false /* isVideo */, audioSource, false /* isRepeaterSource */,
                usePCMAudio, 0 /* profileIdc */, 0 /* levelIdc */,
                0 /* constraintSet */, NULL /* numInputBuffers */);
    }

    ALOGW("Unable to instantiate audio source");

    return OK;
}

sp<IGraphicBufferProducer> WifiDisplaySource::PlaybackSession::getSurfaceTexture() {
    return mBufferQueue;
}

void WifiDisplaySource::PlaybackSession::requestIDRFrame() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.valueAt(i);

        track->requestIDRFrame();
    }
}

void WifiDisplaySource::PlaybackSession::notifySessionDead() {
    // Inform WifiDisplaySource of our premature death (wish).
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionDead);
    notify->post();

    mWeAreDead = true;
}

}  // namespace android

