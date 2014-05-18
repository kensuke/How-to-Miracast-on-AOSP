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

#ifndef MEDIA_SENDER_H_

#define MEDIA_SENDER_H_

#include "rtp/RTPSender.h"

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandler.h>
#include <utils/Errors.h>
#include <utils/Vector.h>

namespace android {

struct ABuffer;
struct ANetworkSession;
struct AMessage;
struct IHDCP;
struct TSPacketizer;

// This class facilitates sending of data from one or more media tracks
// through one or more RTP channels, either providing a 1:1 mapping from
// track to RTP channel or muxing all tracks into a single RTP channel and
// using transport stream encapsulation.
// Optionally the (video) data is encrypted using the provided hdcp object.
struct MediaSender : public AHandler {
    enum {
        kWhatInitDone,
        kWhatError,
        kWhatNetworkStall,
        kWhatInformSender,
    };

    MediaSender(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify);

    status_t setHDCP(const sp<IHDCP> &hdcp);

    enum FlagBits {
        FLAG_MANUALLY_PREPEND_SPS_PPS = 1,
    };
    ssize_t addTrack(const sp<AMessage> &format, uint32_t flags);

    // If trackIndex == -1, initialize for transport stream muxing.
    status_t initAsync(
            ssize_t trackIndex,
            const char *remoteHost,
            int32_t remoteRTPPort,
            RTPSender::TransportMode rtpMode,
            int32_t remoteRTCPPort,
            RTPSender::TransportMode rtcpMode,
            int32_t *localRTPPort);

    status_t queueAccessUnit(
            size_t trackIndex, const sp<ABuffer> &accessUnit);

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~MediaSender();

private:
    enum {
        kWhatSenderNotify,
    };

    enum Mode {
        MODE_UNDEFINED,
        MODE_TRANSPORT_STREAM,
        MODE_ELEMENTARY_STREAMS,
    };

    struct TrackInfo {
        sp<AMessage> mFormat;
        uint32_t mFlags;
        sp<RTPSender> mSender;
        List<sp<ABuffer> > mAccessUnits;
        ssize_t mPacketizerTrackIndex;
        bool mIsAudio;
    };

    sp<ANetworkSession> mNetSession;
    sp<AMessage> mNotify;

    sp<IHDCP> mHDCP;

    Mode mMode;
    int32_t mGeneration;

    Vector<TrackInfo> mTrackInfos;

    sp<TSPacketizer> mTSPacketizer;
    sp<RTPSender> mTSSender;
    int64_t mPrevTimeUs;

    size_t mInitDoneCount;

    FILE *mLogFile;

    void onSenderNotify(const sp<AMessage> &msg);

    void notifyInitDone(status_t err);
    void notifyError(status_t err);
    void notifyNetworkStall(size_t numBytesQueued);

    status_t packetizeAccessUnit(
            size_t trackIndex,
            sp<ABuffer> accessUnit,
            sp<ABuffer> *tsPackets);

    DISALLOW_EVIL_CONSTRUCTORS(MediaSender);
};

}  // namespace android

#endif  // MEDIA_SENDER_H_

