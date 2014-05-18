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

#ifndef WIFI_DISPLAY_SINK_H_

#define WIFI_DISPLAY_SINK_H_

#include "VideoFormats.h"

#include <gui/Surface.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ANetworkSession.h>

namespace android {

struct AMessage;
struct DirectRenderer;
struct MediaReceiver;
struct ParsedMessage;
struct TimeSyncer;

// Represents the RTSP client acting as a wifi display sink.
// Connects to a wifi display source and renders the incoming
// transport stream using a MediaPlayer instance.
struct WifiDisplaySink : public AHandler {
    enum {
        kWhatDisconnected,
    };

    enum Flags {
        FLAG_SPECIAL_MODE = 1,
    };

    // If no notification message is specified (notify == NULL)
    // the sink will stop its looper() once the session ends,
    // otherwise it will post an appropriate notification but leave
    // the looper() running.
    WifiDisplaySink(
            uint32_t flags,
            const sp<ANetworkSession> &netSession,
            const sp<IGraphicBufferProducer> &bufferProducer = NULL,
            const sp<AMessage> &notify = NULL);

    void start(const char *sourceHost, int32_t sourcePort);
    void start(const char *uri);

protected:
    virtual ~WifiDisplaySink();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum State {
        UNDEFINED,
        CONNECTING,
        CONNECTED,
        PAUSED,
        PLAYING,
    };

    enum {
        kWhatStart,
        kWhatRTSPNotify,
        kWhatStop,
        kWhatMediaReceiverNotify,
        kWhatTimeSyncerNotify,
        kWhatReportLateness,
    };

    struct ResponseID {
        int32_t mSessionID;
        int32_t mCSeq;

        bool operator<(const ResponseID &other) const {
            return mSessionID < other.mSessionID
                || (mSessionID == other.mSessionID
                        && mCSeq < other.mCSeq);
        }
    };

    typedef status_t (WifiDisplaySink::*HandleRTSPResponseFunc)(
            int32_t sessionID, const sp<ParsedMessage> &msg);

    static const int64_t kReportLatenessEveryUs = 1000000ll;

    static const AString sUserAgent;

    State mState;
    uint32_t mFlags;
    VideoFormats mSinkSupportedVideoFormats;
    sp<ANetworkSession> mNetSession;
    sp<IGraphicBufferProducer> mSurfaceTex;
    sp<AMessage> mNotify;
    sp<TimeSyncer> mTimeSyncer;
    bool mUsingTCPTransport;
    bool mUsingTCPInterleaving;
    AString mRTSPHost;
    int32_t mSessionID;

    int32_t mNextCSeq;

    KeyedVector<ResponseID, HandleRTSPResponseFunc> mResponseHandlers;

    sp<ALooper> mMediaReceiverLooper;
    sp<MediaReceiver> mMediaReceiver;
    sp<DirectRenderer> mRenderer;

    AString mPlaybackSessionID;
    int32_t mPlaybackSessionTimeoutSecs;

    bool mIDRFrameRequestPending;

    int64_t mTimeOffsetUs;
    bool mTimeOffsetValid;

    bool mSetupDeferred;

    size_t mLatencyCount;
    int64_t mLatencySumUs;
    int64_t mLatencyMaxUs;

    int64_t mMaxDelayMs;

    status_t sendM2(int32_t sessionID);
    status_t sendSetup(int32_t sessionID, const char *uri);
    status_t sendPlay(int32_t sessionID, const char *uri);
    status_t sendIDRFrameRequest(int32_t sessionID);

    status_t onReceiveM2Response(
            int32_t sessionID, const sp<ParsedMessage> &msg);

    status_t onReceiveSetupResponse(
            int32_t sessionID, const sp<ParsedMessage> &msg);

    status_t configureTransport(const sp<ParsedMessage> &msg);

    status_t onReceivePlayResponse(
            int32_t sessionID, const sp<ParsedMessage> &msg);

    status_t onReceiveIDRFrameRequestResponse(
            int32_t sessionID, const sp<ParsedMessage> &msg);

    void registerResponseHandler(
            int32_t sessionID, int32_t cseq, HandleRTSPResponseFunc func);

    void onReceiveClientData(const sp<AMessage> &msg);

    void onOptionsRequest(
            int32_t sessionID,
            int32_t cseq,
            const sp<ParsedMessage> &data);

    void onGetParameterRequest(
            int32_t sessionID,
            int32_t cseq,
            const sp<ParsedMessage> &data);

    void onSetParameterRequest(
            int32_t sessionID,
            int32_t cseq,
            const sp<ParsedMessage> &data);

    void onMediaReceiverNotify(const sp<AMessage> &msg);

    void sendErrorResponse(
            int32_t sessionID,
            const char *errorDetail,
            int32_t cseq);

    static void AppendCommonResponse(AString *response, int32_t cseq);

    bool ParseURL(
            const char *url, AString *host, int32_t *port, AString *path,
            AString *user, AString *pass);

    void dumpDelay(size_t trackIndex, int64_t timeUs);

    DISALLOW_EVIL_CONSTRUCTORS(WifiDisplaySink);
};

}  // namespace android

#endif  // WIFI_DISPLAY_SINK_H_
