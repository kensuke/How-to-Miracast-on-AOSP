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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaSender"
#include <utils/Log.h>

#include "MediaSender.h"

#include "rtp/RTPSender.h"
#include "source/TSPacketizer.h"

#include "include/avc_utils.h"

#include <media/IHDCP.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>
#include <ui/GraphicBuffer.h>

namespace android {

MediaSender::MediaSender(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify)
    : mNetSession(netSession),
      mNotify(notify),
      mMode(MODE_UNDEFINED),
      mGeneration(0),
      mPrevTimeUs(-1ll),
      mInitDoneCount(0),
      mLogFile(NULL) {
    // mLogFile = fopen("/data/misc/log.ts", "wb");
}

MediaSender::~MediaSender() {
    if (mLogFile != NULL) {
        fclose(mLogFile);
        mLogFile = NULL;
    }
}

status_t MediaSender::setHDCP(const sp<IHDCP> &hdcp) {
    if (mMode != MODE_UNDEFINED) {
        return INVALID_OPERATION;
    }

    mHDCP = hdcp;

    return OK;
}

ssize_t MediaSender::addTrack(const sp<AMessage> &format, uint32_t flags) {
    if (mMode != MODE_UNDEFINED) {
        return INVALID_OPERATION;
    }

    TrackInfo info;
    info.mFormat = format;
    info.mFlags = flags;
    info.mPacketizerTrackIndex = -1;

    AString mime;
    CHECK(format->findString("mime", &mime));
    info.mIsAudio = !strncasecmp("audio/", mime.c_str(), 6);

    size_t index = mTrackInfos.size();
    mTrackInfos.push_back(info);

    return index;
}

status_t MediaSender::initAsync(
        ssize_t trackIndex,
        const char *remoteHost,
        int32_t remoteRTPPort,
        RTPSender::TransportMode rtpMode,
        int32_t remoteRTCPPort,
        RTPSender::TransportMode rtcpMode,
        int32_t *localRTPPort) {
    if (trackIndex < 0) {
        if (mMode != MODE_UNDEFINED) {
            return INVALID_OPERATION;
        }

        uint32_t flags = 0;
        if (mHDCP != NULL) {
            // XXX Determine proper HDCP version.
            flags |= TSPacketizer::EMIT_HDCP20_DESCRIPTOR;
        }
        mTSPacketizer = new TSPacketizer(flags);

        status_t err = OK;
        for (size_t i = 0; i < mTrackInfos.size(); ++i) {
            TrackInfo *info = &mTrackInfos.editItemAt(i);

            ssize_t packetizerTrackIndex =
                mTSPacketizer->addTrack(info->mFormat);

            if (packetizerTrackIndex < 0) {
                err = packetizerTrackIndex;
                break;
            }

            info->mPacketizerTrackIndex = packetizerTrackIndex;
        }

        if (err == OK) {
            sp<AMessage> notify = new AMessage(kWhatSenderNotify, id());
            notify->setInt32("generation", mGeneration);
            mTSSender = new RTPSender(mNetSession, notify);
            looper()->registerHandler(mTSSender);

            err = mTSSender->initAsync(
                    remoteHost,
                    remoteRTPPort,
                    rtpMode,
                    remoteRTCPPort,
                    rtcpMode,
                    localRTPPort);

            if (err != OK) {
                looper()->unregisterHandler(mTSSender->id());
                mTSSender.clear();
            }
        }

        if (err != OK) {
            for (size_t i = 0; i < mTrackInfos.size(); ++i) {
                TrackInfo *info = &mTrackInfos.editItemAt(i);
                info->mPacketizerTrackIndex = -1;
            }

            mTSPacketizer.clear();
            return err;
        }

        mMode = MODE_TRANSPORT_STREAM;
        mInitDoneCount = 1;

        return OK;
    }

    if (mMode == MODE_TRANSPORT_STREAM) {
        return INVALID_OPERATION;
    }

    if ((size_t)trackIndex >= mTrackInfos.size()) {
        return -ERANGE;
    }

    TrackInfo *info = &mTrackInfos.editItemAt(trackIndex);

    if (info->mSender != NULL) {
        return INVALID_OPERATION;
    }

    sp<AMessage> notify = new AMessage(kWhatSenderNotify, id());
    notify->setInt32("generation", mGeneration);
    notify->setSize("trackIndex", trackIndex);

    info->mSender = new RTPSender(mNetSession, notify);
    looper()->registerHandler(info->mSender);

    status_t err = info->mSender->initAsync(
            remoteHost,
            remoteRTPPort,
            rtpMode,
            remoteRTCPPort,
            rtcpMode,
            localRTPPort);

    if (err != OK) {
        looper()->unregisterHandler(info->mSender->id());
        info->mSender.clear();

        return err;
    }

    if (mMode == MODE_UNDEFINED) {
        mInitDoneCount = mTrackInfos.size();
    }

    mMode = MODE_ELEMENTARY_STREAMS;

    return OK;
}

status_t MediaSender::queueAccessUnit(
        size_t trackIndex, const sp<ABuffer> &accessUnit) {
    if (mMode == MODE_UNDEFINED) {
        return INVALID_OPERATION;
    }

    if (trackIndex >= mTrackInfos.size()) {
        return -ERANGE;
    }

    if (mMode == MODE_TRANSPORT_STREAM) {
        TrackInfo *info = &mTrackInfos.editItemAt(trackIndex);
        info->mAccessUnits.push_back(accessUnit);

        mTSPacketizer->extractCSDIfNecessary(info->mPacketizerTrackIndex);

        for (;;) {
            ssize_t minTrackIndex = -1;
            int64_t minTimeUs = -1ll;

            for (size_t i = 0; i < mTrackInfos.size(); ++i) {
                const TrackInfo &info = mTrackInfos.itemAt(i);

                if (info.mAccessUnits.empty()) {
                    minTrackIndex = -1;
                    minTimeUs = -1ll;
                    break;
                }

                int64_t timeUs;
                const sp<ABuffer> &accessUnit = *info.mAccessUnits.begin();
                CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

                if (minTrackIndex < 0 || timeUs < minTimeUs) {
                    minTrackIndex = i;
                    minTimeUs = timeUs;
                }
            }

            if (minTrackIndex < 0) {
                return OK;
            }

            TrackInfo *info = &mTrackInfos.editItemAt(minTrackIndex);
            sp<ABuffer> accessUnit = *info->mAccessUnits.begin();
            info->mAccessUnits.erase(info->mAccessUnits.begin());

            sp<ABuffer> tsPackets;
            status_t err = packetizeAccessUnit(
                    minTrackIndex, accessUnit, &tsPackets);

            if (err == OK) {
                if (mLogFile != NULL) {
                    fwrite(tsPackets->data(), 1, tsPackets->size(), mLogFile);
                }

                int64_t timeUs;
                CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));
                tsPackets->meta()->setInt64("timeUs", timeUs);

                err = mTSSender->queueBuffer(
                        tsPackets,
                        33 /* packetType */,
                        RTPSender::PACKETIZATION_TRANSPORT_STREAM);
            }

            if (err != OK) {
                return err;
            }
        }
    }

    TrackInfo *info = &mTrackInfos.editItemAt(trackIndex);

    return info->mSender->queueBuffer(
            accessUnit,
            info->mIsAudio ? 96 : 97 /* packetType */,
            info->mIsAudio
                ? RTPSender::PACKETIZATION_AAC : RTPSender::PACKETIZATION_H264);
}

void MediaSender::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSenderNotify:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mGeneration) {
                break;
            }

            onSenderNotify(msg);
            break;
        }

        default:
            TRESPASS();
    }
}

void MediaSender::onSenderNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case RTPSender::kWhatInitDone:
        {
            --mInitDoneCount;

            int32_t err;
            CHECK(msg->findInt32("err", &err));

            if (err != OK) {
                notifyInitDone(err);
                ++mGeneration;
                break;
            }

            if (mInitDoneCount == 0) {
                notifyInitDone(OK);
            }
            break;
        }

        case RTPSender::kWhatError:
        {
            int32_t err;
            CHECK(msg->findInt32("err", &err));

            notifyError(err);
            break;
        }

        case kWhatNetworkStall:
        {
            size_t numBytesQueued;
            CHECK(msg->findSize("numBytesQueued", &numBytesQueued));

            notifyNetworkStall(numBytesQueued);
            break;
        }

        case kWhatInformSender:
        {
            int64_t avgLatencyUs;
            CHECK(msg->findInt64("avgLatencyUs", &avgLatencyUs));

            int64_t maxLatencyUs;
            CHECK(msg->findInt64("maxLatencyUs", &maxLatencyUs));

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatInformSender);
            notify->setInt64("avgLatencyUs", avgLatencyUs);
            notify->setInt64("maxLatencyUs", maxLatencyUs);
            notify->post();
            break;
        }

        default:
            TRESPASS();
    }
}

void MediaSender::notifyInitDone(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatInitDone);
    notify->setInt32("err", err);
    notify->post();
}

void MediaSender::notifyError(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

void MediaSender::notifyNetworkStall(size_t numBytesQueued) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatNetworkStall);
    notify->setSize("numBytesQueued", numBytesQueued);
    notify->post();
}

status_t MediaSender::packetizeAccessUnit(
        size_t trackIndex,
        sp<ABuffer> accessUnit,
        sp<ABuffer> *tsPackets) {
    const TrackInfo &info = mTrackInfos.itemAt(trackIndex);

    uint32_t flags = 0;

    bool isHDCPEncrypted = false;
    uint64_t inputCTR;
    uint8_t HDCP_private_data[16];

    bool manuallyPrependSPSPPS =
        !info.mIsAudio
        && (info.mFlags & FLAG_MANUALLY_PREPEND_SPS_PPS)
        && IsIDR(accessUnit);

    if (mHDCP != NULL && !info.mIsAudio) {
        isHDCPEncrypted = true;

        if (manuallyPrependSPSPPS) {
            accessUnit = mTSPacketizer->prependCSD(
                    info.mPacketizerTrackIndex, accessUnit);
        }

        status_t err;
        native_handle_t* handle;
        if (accessUnit->meta()->findPointer("handle", (void**)&handle)
                && handle != NULL) {
            int32_t rangeLength, rangeOffset;
            sp<AMessage> notify;
            CHECK(accessUnit->meta()->findInt32("rangeOffset", &rangeOffset));
            CHECK(accessUnit->meta()->findInt32("rangeLength", &rangeLength));
            CHECK(accessUnit->meta()->findMessage("notify", &notify)
                    && notify != NULL);
            CHECK_GE(accessUnit->size(), rangeLength);

            sp<GraphicBuffer> grbuf(new GraphicBuffer(
                    rangeOffset + rangeLength, 1, HAL_PIXEL_FORMAT_Y8,
                    GRALLOC_USAGE_HW_VIDEO_ENCODER, rangeOffset + rangeLength,
                    handle, false));

            err = mHDCP->encryptNative(
                    grbuf, rangeOffset, rangeLength,
                    trackIndex  /* streamCTR */,
                    &inputCTR,
                    accessUnit->data());
            notify->post();
        } else {
            err = mHDCP->encrypt(
                    accessUnit->data(), accessUnit->size(),
                    trackIndex  /* streamCTR */,
                    &inputCTR,
                    accessUnit->data());
        }

        if (err != OK) {
            ALOGE("Failed to HDCP-encrypt media data (err %d)",
                  err);

            return err;
        }

        HDCP_private_data[0] = 0x00;

        HDCP_private_data[1] =
            (((trackIndex >> 30) & 3) << 1) | 1;

        HDCP_private_data[2] = (trackIndex >> 22) & 0xff;

        HDCP_private_data[3] =
            (((trackIndex >> 15) & 0x7f) << 1) | 1;

        HDCP_private_data[4] = (trackIndex >> 7) & 0xff;

        HDCP_private_data[5] =
            ((trackIndex & 0x7f) << 1) | 1;

        HDCP_private_data[6] = 0x00;

        HDCP_private_data[7] =
            (((inputCTR >> 60) & 0x0f) << 1) | 1;

        HDCP_private_data[8] = (inputCTR >> 52) & 0xff;

        HDCP_private_data[9] =
            (((inputCTR >> 45) & 0x7f) << 1) | 1;

        HDCP_private_data[10] = (inputCTR >> 37) & 0xff;

        HDCP_private_data[11] =
            (((inputCTR >> 30) & 0x7f) << 1) | 1;

        HDCP_private_data[12] = (inputCTR >> 22) & 0xff;

        HDCP_private_data[13] =
            (((inputCTR >> 15) & 0x7f) << 1) | 1;

        HDCP_private_data[14] = (inputCTR >> 7) & 0xff;

        HDCP_private_data[15] =
            ((inputCTR & 0x7f) << 1) | 1;

        flags |= TSPacketizer::IS_ENCRYPTED;
    } else if (manuallyPrependSPSPPS) {
        flags |= TSPacketizer::PREPEND_SPS_PPS_TO_IDR_FRAMES;
    }

    int64_t timeUs = ALooper::GetNowUs();
    if (mPrevTimeUs < 0ll || mPrevTimeUs + 100000ll <= timeUs) {
        flags |= TSPacketizer::EMIT_PCR;
        flags |= TSPacketizer::EMIT_PAT_AND_PMT;

        mPrevTimeUs = timeUs;
    }

    mTSPacketizer->packetize(
            info.mPacketizerTrackIndex,
            accessUnit,
            tsPackets,
            flags,
            !isHDCPEncrypted ? NULL : HDCP_private_data,
            !isHDCPEncrypted ? 0 : sizeof(HDCP_private_data),
            info.mIsAudio ? 2 : 0 /* numStuffingBytes */);

    return OK;
}

}  // namespace android

