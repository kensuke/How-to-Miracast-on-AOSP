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
#define LOG_TAG "TunnelRenderer"
#include <utils/Log.h>

#include "TunnelRenderer.h"

#include "ATSParser.h"

#include <binder/IMemory.h>
#include <binder/IServiceManager.h>
#include <gui/SurfaceComposerClient.h>
#include <media/IMediaPlayerService.h>
#include <media/IStreamSource.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <ui/DisplayInfo.h>

namespace android {

struct TunnelRenderer::PlayerClient : public BnMediaPlayerClient {
    PlayerClient() {}

    virtual void notify(int msg, int ext1, int ext2, const Parcel *obj) {
        ALOGI("notify %d, %d, %d", msg, ext1, ext2);
    }

protected:
    virtual ~PlayerClient() {}

private:
    DISALLOW_EVIL_CONSTRUCTORS(PlayerClient);
};

struct TunnelRenderer::StreamSource : public BnStreamSource {
    StreamSource(TunnelRenderer *owner);

    virtual void setListener(const sp<IStreamListener> &listener);
    virtual void setBuffers(const Vector<sp<IMemory> > &buffers);

    virtual void onBufferAvailable(size_t index);

    virtual uint32_t flags() const;

    void doSomeWork();

protected:
    virtual ~StreamSource();

private:
    mutable Mutex mLock;

    TunnelRenderer *mOwner;

    sp<IStreamListener> mListener;

    Vector<sp<IMemory> > mBuffers;
    List<size_t> mIndicesAvailable;

    size_t mNumDeqeued;

    DISALLOW_EVIL_CONSTRUCTORS(StreamSource);
};

////////////////////////////////////////////////////////////////////////////////

TunnelRenderer::StreamSource::StreamSource(TunnelRenderer *owner)
    : mOwner(owner),
      mNumDeqeued(0) {
}

TunnelRenderer::StreamSource::~StreamSource() {
}

void TunnelRenderer::StreamSource::setListener(
        const sp<IStreamListener> &listener) {
    mListener = listener;
}

void TunnelRenderer::StreamSource::setBuffers(
        const Vector<sp<IMemory> > &buffers) {
    mBuffers = buffers;
}

void TunnelRenderer::StreamSource::onBufferAvailable(size_t index) {
    CHECK_LT(index, mBuffers.size());

    {
        Mutex::Autolock autoLock(mLock);
        mIndicesAvailable.push_back(index);
    }

    doSomeWork();
}

uint32_t TunnelRenderer::StreamSource::flags() const {
    return kFlagAlignedVideoData;
}

void TunnelRenderer::StreamSource::doSomeWork() {
    Mutex::Autolock autoLock(mLock);

    while (!mIndicesAvailable.empty()) {
        sp<ABuffer> srcBuffer = mOwner->dequeueBuffer();
        if (srcBuffer == NULL) {
            break;
        }

        ++mNumDeqeued;

        if (mNumDeqeued == 1) {
            ALOGI("fixing real time now.");

            sp<AMessage> extra = new AMessage;

            extra->setInt32(
                    IStreamListener::kKeyDiscontinuityMask,
                    ATSParser::DISCONTINUITY_ABSOLUTE_TIME);

            extra->setInt64("timeUs", ALooper::GetNowUs());

            mListener->issueCommand(
                    IStreamListener::DISCONTINUITY,
                    false /* synchronous */,
                    extra);
        }

        ALOGV("dequeue TS packet of size %d", srcBuffer->size());

        size_t index = *mIndicesAvailable.begin();
        mIndicesAvailable.erase(mIndicesAvailable.begin());

        sp<IMemory> mem = mBuffers.itemAt(index);
        CHECK_LE(srcBuffer->size(), mem->size());
        CHECK_EQ((srcBuffer->size() % 188), 0u);

        memcpy(mem->pointer(), srcBuffer->data(), srcBuffer->size());
        mListener->queueBuffer(index, srcBuffer->size());
    }
}

////////////////////////////////////////////////////////////////////////////////

TunnelRenderer::TunnelRenderer(
        const sp<AMessage> &notifyLost,
        const sp<ISurfaceTexture> &surfaceTex)
    : mNotifyLost(notifyLost),
      mSurfaceTex(surfaceTex),
      mTotalBytesQueued(0ll),
      mLastDequeuedExtSeqNo(-1),
      mFirstFailedAttemptUs(-1ll),
      mRequestedRetransmission(false) {
}

TunnelRenderer::~TunnelRenderer() {
    destroyPlayer();
}

void TunnelRenderer::queueBuffer(const sp<ABuffer> &buffer) {
    Mutex::Autolock autoLock(mLock);

    mTotalBytesQueued += buffer->size();

    if (mPackets.empty()) {
        mPackets.push_back(buffer);
        return;
    }

    int32_t newExtendedSeqNo = buffer->int32Data();

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
            mPackets.insert(++it, buffer);
            return;
        }

        if (it == firstIt) {
            // Insert new packet before the first existing one.
            mPackets.insert(it, buffer);
            return;
        }

        --it;
    }
}

sp<ABuffer> TunnelRenderer::dequeueBuffer() {
    Mutex::Autolock autoLock(mLock);

    sp<ABuffer> buffer;
    int32_t extSeqNo;
    while (!mPackets.empty()) {
        buffer = *mPackets.begin();
        extSeqNo = buffer->int32Data();

        if (mLastDequeuedExtSeqNo < 0 || extSeqNo > mLastDequeuedExtSeqNo) {
            break;
        }

        // This is a retransmission of a packet we've already returned.

        mTotalBytesQueued -= buffer->size();
        buffer.clear();
        extSeqNo = -1;

        mPackets.erase(mPackets.begin());
    }

    if (mPackets.empty()) {
        if (mFirstFailedAttemptUs < 0ll) {
            mFirstFailedAttemptUs = ALooper::GetNowUs();
            mRequestedRetransmission = false;
        } else {
            ALOGV("no packets available for %.2f secs",
                    (ALooper::GetNowUs() - mFirstFailedAttemptUs) / 1E6);
        }

        return NULL;
    }

    if (mLastDequeuedExtSeqNo < 0 || extSeqNo == mLastDequeuedExtSeqNo + 1) {
        if (mRequestedRetransmission) {
            ALOGI("Recovered after requesting retransmission of %d",
                  extSeqNo);
        }

        mLastDequeuedExtSeqNo = extSeqNo;
        mFirstFailedAttemptUs = -1ll;
        mRequestedRetransmission = false;

        mPackets.erase(mPackets.begin());

        mTotalBytesQueued -= buffer->size();

        return buffer;
    }

    if (mFirstFailedAttemptUs < 0ll) {
        mFirstFailedAttemptUs = ALooper::GetNowUs();

        ALOGI("failed to get the correct packet the first time.");
        return NULL;
    }

    if (mFirstFailedAttemptUs + 50000ll > ALooper::GetNowUs()) {
        // We're willing to wait a little while to get the right packet.

        if (!mRequestedRetransmission) {
            ALOGI("requesting retransmission of seqNo %d",
                  (mLastDequeuedExtSeqNo + 1) & 0xffff);

            sp<AMessage> notify = mNotifyLost->dup();
            notify->setInt32("seqNo", (mLastDequeuedExtSeqNo + 1) & 0xffff);
            notify->post();

            mRequestedRetransmission = true;
        } else {
            ALOGI("still waiting for the correct packet to arrive.");
        }

        return NULL;
    }

    ALOGI("dropping packet. extSeqNo %d didn't arrive in time",
            mLastDequeuedExtSeqNo + 1);

    // Permanent failure, we never received the packet.
    mLastDequeuedExtSeqNo = extSeqNo;
    mFirstFailedAttemptUs = -1ll;
    mRequestedRetransmission = false;

    mTotalBytesQueued -= buffer->size();

    mPackets.erase(mPackets.begin());

    return buffer;
}

void TunnelRenderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatQueueBuffer:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            queueBuffer(buffer);

            if (mStreamSource == NULL) {
                if (mTotalBytesQueued > 0ll) {
                    initPlayer();
                } else {
                    ALOGI("Have %lld bytes queued...", mTotalBytesQueued);
                }
            } else {
                mStreamSource->doSomeWork();
            }
            break;
        }

        default:
            TRESPASS();
    }
}

void TunnelRenderer::initPlayer() {
    if (mSurfaceTex == NULL) {
        mComposerClient = new SurfaceComposerClient;
        CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);

        DisplayInfo info;
        SurfaceComposerClient::getDisplayInfo(0, &info);
        ssize_t displayWidth = info.w;
        ssize_t displayHeight = info.h;

        mSurfaceControl =
            mComposerClient->createSurface(
                    String8("A Surface"),
                    displayWidth,
                    displayHeight,
                    PIXEL_FORMAT_RGB_565,
                    0);

        CHECK(mSurfaceControl != NULL);
        CHECK(mSurfaceControl->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        CHECK_EQ(mSurfaceControl->setLayer(INT_MAX), (status_t)OK);
        CHECK_EQ(mSurfaceControl->show(), (status_t)OK);
        SurfaceComposerClient::closeGlobalTransaction();

        mSurface = mSurfaceControl->getSurface();
        CHECK(mSurface != NULL);
    }

    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    CHECK(service.get() != NULL);

    mStreamSource = new StreamSource(this);

    mPlayerClient = new PlayerClient;

    mPlayer = service->create(getpid(), mPlayerClient, 0);
    CHECK(mPlayer != NULL);
    CHECK_EQ(mPlayer->setDataSource(mStreamSource), (status_t)OK);

    mPlayer->setVideoSurfaceTexture(
            mSurfaceTex != NULL ? mSurfaceTex : mSurface->getSurfaceTexture());

    mPlayer->start();
}

void TunnelRenderer::destroyPlayer() {
    mStreamSource.clear();

    mPlayer->stop();
    mPlayer.clear();

    if (mSurfaceTex == NULL) {
        mSurface.clear();
        mSurfaceControl.clear();

        mComposerClient->dispose();
        mComposerClient.clear();
    }
}

}  // namespace android

