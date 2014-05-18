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

#ifndef DIRECT_RENDERER_H_

#define DIRECT_RENDERER_H_

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct IGraphicBufferProducer;

// Renders audio and video data queued by calls to "queueAccessUnit".
struct DirectRenderer : public AHandler {
    DirectRenderer(const sp<IGraphicBufferProducer> &bufferProducer);

    void setFormat(size_t trackIndex, const sp<AMessage> &format);
    void queueAccessUnit(size_t trackIndex, const sp<ABuffer> &accessUnit);

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~DirectRenderer();

private:
    struct DecoderContext;
    struct AudioRenderer;

    enum {
        kWhatDecoderNotify,
        kWhatRenderVideo,
        kWhatQueueAccessUnit,
        kWhatSetFormat,
    };

    struct OutputInfo {
        size_t mIndex;
        int64_t mTimeUs;
        sp<ABuffer> mBuffer;
    };

    sp<IGraphicBufferProducer> mSurfaceTex;

    sp<DecoderContext> mDecoderContext[2];
    List<OutputInfo> mVideoOutputBuffers;

    bool mVideoRenderPending;

    sp<AudioRenderer> mAudioRenderer;

    int32_t mNumFramesLate;
    int32_t mNumFrames;

    void onDecoderNotify(const sp<AMessage> &msg);

    void queueOutputBuffer(
            size_t trackIndex,
            size_t index, int64_t timeUs, const sp<ABuffer> &buffer);

    void scheduleVideoRenderIfNecessary();
    void onRenderVideo();

    void onSetFormat(const sp<AMessage> &msg);
    void onQueueAccessUnit(const sp<AMessage> &msg);

    void internalSetFormat(size_t trackIndex, const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(DirectRenderer);
};

}  // namespace android

#endif  // DIRECT_RENDERER_H_
