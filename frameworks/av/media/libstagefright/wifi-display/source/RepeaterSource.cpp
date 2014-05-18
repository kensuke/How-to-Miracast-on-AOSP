//#define LOG_NDEBUG 0
#define LOG_TAG "RepeaterSource"
#include <utils/Log.h>

#include "RepeaterSource.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>

namespace android {

RepeaterSource::RepeaterSource(const sp<MediaSource> &source, double rateHz)
    : mStarted(false),
      mSource(source),
      mRateHz(rateHz),
      mBuffer(NULL),
      mResult(OK),
      mLastBufferUpdateUs(-1ll),
      mStartTimeUs(-1ll),
      mFrameCount(0) {
}

RepeaterSource::~RepeaterSource() {
    CHECK(!mStarted);
}

double RepeaterSource::getFrameRate() const {
    return mRateHz;
}

void RepeaterSource::setFrameRate(double rateHz) {
    Mutex::Autolock autoLock(mLock);

    if (rateHz == mRateHz) {
        return;
    }

    if (mStartTimeUs >= 0ll) {
        int64_t nextTimeUs = mStartTimeUs + (mFrameCount * 1000000ll) / mRateHz;
        mStartTimeUs = nextTimeUs;
        mFrameCount = 0;
    }
    mRateHz = rateHz;
}

status_t RepeaterSource::start(MetaData *params) {
    CHECK(!mStarted);

    status_t err = mSource->start(params);

    if (err != OK) {
        return err;
    }

    mBuffer = NULL;
    mResult = OK;
    mStartTimeUs = -1ll;
    mFrameCount = 0;

    mLooper = new ALooper;
    mLooper->setName("repeater_looper");
    mLooper->start();

    mReflector = new AHandlerReflector<RepeaterSource>(this);
    mLooper->registerHandler(mReflector);

    postRead();

    mStarted = true;

    return OK;
}

status_t RepeaterSource::stop() {
    CHECK(mStarted);

    ALOGV("stopping");

    if (mLooper != NULL) {
        mLooper->stop();
        mLooper.clear();

        mReflector.clear();
    }

    if (mBuffer != NULL) {
        ALOGV("releasing mbuf %p", mBuffer);
        mBuffer->release();
        mBuffer = NULL;
    }

    status_t err = mSource->stop();

    ALOGV("stopped");

    mStarted = false;

    return err;
}

sp<MetaData> RepeaterSource::getFormat() {
    return mSource->getFormat();
}

status_t RepeaterSource::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    int64_t seekTimeUs;
    ReadOptions::SeekMode seekMode;
    CHECK(options == NULL || !options->getSeekTo(&seekTimeUs, &seekMode));

    for (;;) {
        int64_t bufferTimeUs = -1ll;

        if (mStartTimeUs < 0ll) {
            Mutex::Autolock autoLock(mLock);
            while ((mLastBufferUpdateUs < 0ll || mBuffer == NULL)
                    && mResult == OK) {
                mCondition.wait(mLock);
            }

            ALOGV("now resuming.");
            mStartTimeUs = ALooper::GetNowUs();
            bufferTimeUs = mStartTimeUs;
        } else {
            bufferTimeUs = mStartTimeUs + (mFrameCount * 1000000ll) / mRateHz;

            int64_t nowUs = ALooper::GetNowUs();
            int64_t delayUs = bufferTimeUs - nowUs;

            if (delayUs > 0ll) {
                usleep(delayUs);
            }
        }

        bool stale = false;

        {
            Mutex::Autolock autoLock(mLock);
            if (mResult != OK) {
                CHECK(mBuffer == NULL);
                return mResult;
            }

#if SUSPEND_VIDEO_IF_IDLE
            int64_t nowUs = ALooper::GetNowUs();
            if (nowUs - mLastBufferUpdateUs > 1000000ll) {
                mLastBufferUpdateUs = -1ll;
                stale = true;
            } else
#endif
            {
                mBuffer->add_ref();
                *buffer = mBuffer;
                (*buffer)->meta_data()->setInt64(kKeyTime, bufferTimeUs);
                ++mFrameCount;
            }
        }

        if (!stale) {
            break;
        }

        mStartTimeUs = -1ll;
        mFrameCount = 0;
        ALOGV("now dormant");
    }

    return OK;
}

void RepeaterSource::postRead() {
    (new AMessage(kWhatRead, mReflector->id()))->post();
}

void RepeaterSource::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRead:
        {
            MediaBuffer *buffer;
            status_t err = mSource->read(&buffer);

            ALOGV("read mbuf %p", buffer);

            Mutex::Autolock autoLock(mLock);
            if (mBuffer != NULL) {
                mBuffer->release();
                mBuffer = NULL;
            }
            mBuffer = buffer;
            mResult = err;
            mLastBufferUpdateUs = ALooper::GetNowUs();

            mCondition.broadcast();

            if (err == OK) {
                postRead();
            }
            break;
        }

        default:
            TRESPASS();
    }
}

void RepeaterSource::wakeUp() {
    ALOGV("wakeUp");
    Mutex::Autolock autoLock(mLock);
    if (mLastBufferUpdateUs < 0ll && mBuffer != NULL) {
        mLastBufferUpdateUs = ALooper::GetNowUs();
        mCondition.broadcast();
    }
}

}  // namespace android
