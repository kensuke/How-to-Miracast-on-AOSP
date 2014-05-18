#ifndef REPEATER_SOURCE_H_

#define REPEATER_SOURCE_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandlerReflector.h>
#include <media/stagefright/MediaSource.h>

#define SUSPEND_VIDEO_IF_IDLE   0

namespace android {

// This MediaSource delivers frames at a constant rate by repeating buffers
// if necessary.
struct RepeaterSource : public MediaSource {
    RepeaterSource(const sp<MediaSource> &source, double rateHz);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

    void onMessageReceived(const sp<AMessage> &msg);

    // If RepeaterSource is currently dormant, because SurfaceFlinger didn't
    // send updates in a while, this is its wakeup call.
    void wakeUp();

    double getFrameRate() const;
    void setFrameRate(double rateHz);

protected:
    virtual ~RepeaterSource();

private:
    enum {
        kWhatRead,
    };

    Mutex mLock;
    Condition mCondition;

    bool mStarted;

    sp<MediaSource> mSource;
    double mRateHz;

    sp<ALooper> mLooper;
    sp<AHandlerReflector<RepeaterSource> > mReflector;

    MediaBuffer *mBuffer;
    status_t mResult;
    int64_t mLastBufferUpdateUs;

    int64_t mStartTimeUs;
    int32_t mFrameCount;

    void postRead();

    DISALLOW_EVIL_CONSTRUCTORS(RepeaterSource);
};

}  // namespace android

#endif // REPEATER_SOURCE_H_
