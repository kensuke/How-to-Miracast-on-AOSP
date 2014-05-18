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
#define LOG_TAG "TSPacketizer"
#include <utils/Log.h>

#include "TSPacketizer.h"
#include "include/avc_utils.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>

#include <arpa/inet.h>

namespace android {

struct TSPacketizer::Track : public RefBase {
    Track(const sp<AMessage> &format,
          unsigned PID, unsigned streamType, unsigned streamID);

    unsigned PID() const;
    unsigned streamType() const;
    unsigned streamID() const;

    // Returns the previous value.
    unsigned incrementContinuityCounter();

    bool isAudio() const;
    bool isVideo() const;

    bool isH264() const;
    bool isAAC() const;
    bool lacksADTSHeader() const;
    bool isPCMAudio() const;

    sp<ABuffer> prependCSD(const sp<ABuffer> &accessUnit) const;
    sp<ABuffer> prependADTSHeader(const sp<ABuffer> &accessUnit) const;

    size_t countDescriptors() const;
    sp<ABuffer> descriptorAt(size_t index) const;

    void finalize();
    void extractCSDIfNecessary();

protected:
    virtual ~Track();

private:
    sp<AMessage> mFormat;

    unsigned mPID;
    unsigned mStreamType;
    unsigned mStreamID;
    unsigned mContinuityCounter;

    AString mMIME;
    Vector<sp<ABuffer> > mCSD;

    Vector<sp<ABuffer> > mDescriptors;

    bool mAudioLacksATDSHeaders;
    bool mFinalized;
    bool mExtractedCSD;

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

TSPacketizer::Track::Track(
        const sp<AMessage> &format,
        unsigned PID, unsigned streamType, unsigned streamID)
    : mFormat(format),
      mPID(PID),
      mStreamType(streamType),
      mStreamID(streamID),
      mContinuityCounter(0),
      mAudioLacksATDSHeaders(false),
      mFinalized(false),
      mExtractedCSD(false) {
    CHECK(format->findString("mime", &mMIME));
}

void TSPacketizer::Track::extractCSDIfNecessary() {
    if (mExtractedCSD) {
        return;
    }

    if (!strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_VIDEO_AVC)
            || !strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        for (size_t i = 0;; ++i) {
            sp<ABuffer> csd;
            if (!mFormat->findBuffer(StringPrintf("csd-%d", i).c_str(), &csd)) {
                break;
            }

            mCSD.push(csd);
        }

        if (!strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)) {
            int32_t isADTS;
            if (!mFormat->findInt32("is-adts", &isADTS) || isADTS == 0) {
                mAudioLacksATDSHeaders = true;
            }
        }
    }

    mExtractedCSD = true;
}

TSPacketizer::Track::~Track() {
}

unsigned TSPacketizer::Track::PID() const {
    return mPID;
}

unsigned TSPacketizer::Track::streamType() const {
    return mStreamType;
}

unsigned TSPacketizer::Track::streamID() const {
    return mStreamID;
}

unsigned TSPacketizer::Track::incrementContinuityCounter() {
    unsigned prevCounter = mContinuityCounter;

    if (++mContinuityCounter == 16) {
        mContinuityCounter = 0;
    }

    return prevCounter;
}

bool TSPacketizer::Track::isAudio() const {
    return !strncasecmp("audio/", mMIME.c_str(), 6);
}

bool TSPacketizer::Track::isVideo() const {
    return !strncasecmp("video/", mMIME.c_str(), 6);
}

bool TSPacketizer::Track::isH264() const {
    return !strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_VIDEO_AVC);
}

bool TSPacketizer::Track::isAAC() const {
    return !strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_AUDIO_AAC);
}

bool TSPacketizer::Track::isPCMAudio() const {
    return !strcasecmp(mMIME.c_str(), MEDIA_MIMETYPE_AUDIO_RAW);
}

bool TSPacketizer::Track::lacksADTSHeader() const {
    return mAudioLacksATDSHeaders;
}

sp<ABuffer> TSPacketizer::Track::prependCSD(
        const sp<ABuffer> &accessUnit) const {
    size_t size = 0;
    for (size_t i = 0; i < mCSD.size(); ++i) {
        size += mCSD.itemAt(i)->size();
    }

    sp<ABuffer> dup = new ABuffer(accessUnit->size() + size);
    size_t offset = 0;
    for (size_t i = 0; i < mCSD.size(); ++i) {
        const sp<ABuffer> &csd = mCSD.itemAt(i);

        memcpy(dup->data() + offset, csd->data(), csd->size());
        offset += csd->size();
    }

    memcpy(dup->data() + offset, accessUnit->data(), accessUnit->size());

    return dup;
}

sp<ABuffer> TSPacketizer::Track::prependADTSHeader(
        const sp<ABuffer> &accessUnit) const {
    CHECK_EQ(mCSD.size(), 1u);

    const uint8_t *codec_specific_data = mCSD.itemAt(0)->data();

    const uint32_t aac_frame_length = accessUnit->size() + 7;

    sp<ABuffer> dup = new ABuffer(aac_frame_length);

    unsigned profile = (codec_specific_data[0] >> 3) - 1;

    unsigned sampling_freq_index =
        ((codec_specific_data[0] & 7) << 1)
        | (codec_specific_data[1] >> 7);

    unsigned channel_configuration =
        (codec_specific_data[1] >> 3) & 0x0f;

    uint8_t *ptr = dup->data();

    *ptr++ = 0xff;
    *ptr++ = 0xf1;  // b11110001, ID=0, layer=0, protection_absent=1

    *ptr++ =
        profile << 6
        | sampling_freq_index << 2
        | ((channel_configuration >> 2) & 1);  // private_bit=0

    // original_copy=0, home=0, copyright_id_bit=0, copyright_id_start=0
    *ptr++ =
        (channel_configuration & 3) << 6
        | aac_frame_length >> 11;
    *ptr++ = (aac_frame_length >> 3) & 0xff;
    *ptr++ = (aac_frame_length & 7) << 5;

    // adts_buffer_fullness=0, number_of_raw_data_blocks_in_frame=0
    *ptr++ = 0;

    memcpy(ptr, accessUnit->data(), accessUnit->size());

    return dup;
}

size_t TSPacketizer::Track::countDescriptors() const {
    return mDescriptors.size();
}

sp<ABuffer> TSPacketizer::Track::descriptorAt(size_t index) const {
    CHECK_LT(index, mDescriptors.size());
    return mDescriptors.itemAt(index);
}

void TSPacketizer::Track::finalize() {
    if (mFinalized) {
        return;
    }

    if (isH264()) {
        {
            // AVC video descriptor (40)

            sp<ABuffer> descriptor = new ABuffer(6);
            uint8_t *data = descriptor->data();
            data[0] = 40;  // descriptor_tag
            data[1] = 4;  // descriptor_length

            if (mCSD.size() > 0) {
                CHECK_GE(mCSD.size(), 1u);
                const sp<ABuffer> &sps = mCSD.itemAt(0);
                CHECK(!memcmp("\x00\x00\x00\x01", sps->data(), 4));
                CHECK_GE(sps->size(), 7u);
                // profile_idc, constraint_set*, level_idc
                memcpy(&data[2], sps->data() + 4, 3);
            } else {
                int32_t profileIdc, levelIdc, constraintSet;
                CHECK(mFormat->findInt32("profile-idc", &profileIdc));
                CHECK(mFormat->findInt32("level-idc", &levelIdc));
                CHECK(mFormat->findInt32("constraint-set", &constraintSet));
                CHECK_GE(profileIdc, 0u);
                CHECK_GE(levelIdc, 0u);
                data[2] = profileIdc;    // profile_idc
                data[3] = constraintSet; // constraint_set*
                data[4] = levelIdc;      // level_idc
            }

            // AVC_still_present=0, AVC_24_hour_picture_flag=0, reserved
            data[5] = 0x3f;

            mDescriptors.push_back(descriptor);
        }

        {
            // AVC timing and HRD descriptor (42)

            sp<ABuffer> descriptor = new ABuffer(4);
            uint8_t *data = descriptor->data();
            data[0] = 42;  // descriptor_tag
            data[1] = 2;  // descriptor_length

            // hrd_management_valid_flag = 0
            // reserved = 111111b
            // picture_and_timing_info_present = 0

            data[2] = 0x7e;

            // fixed_frame_rate_flag = 0
            // temporal_poc_flag = 0
            // picture_to_display_conversion_flag = 0
            // reserved = 11111b
            data[3] = 0x1f;

            mDescriptors.push_back(descriptor);
        }
    } else if (isPCMAudio()) {
        // LPCM audio stream descriptor (0x83)

        int32_t channelCount;
        CHECK(mFormat->findInt32("channel-count", &channelCount));
        CHECK_EQ(channelCount, 2);

        int32_t sampleRate;
        CHECK(mFormat->findInt32("sample-rate", &sampleRate));
        CHECK(sampleRate == 44100 || sampleRate == 48000);

        sp<ABuffer> descriptor = new ABuffer(4);
        uint8_t *data = descriptor->data();
        data[0] = 0x83;  // descriptor_tag
        data[1] = 2;  // descriptor_length

        unsigned sampling_frequency = (sampleRate == 44100) ? 1 : 2;

        data[2] = (sampling_frequency << 5)
                    | (3 /* reserved */ << 1)
                    | 0 /* emphasis_flag */;

        data[3] =
            (1 /* number_of_channels = stereo */ << 5)
            | 0xf /* reserved */;

        mDescriptors.push_back(descriptor);
    }

    mFinalized = true;
}

////////////////////////////////////////////////////////////////////////////////

TSPacketizer::TSPacketizer(uint32_t flags)
    : mFlags(flags),
      mPATContinuityCounter(0),
      mPMTContinuityCounter(0) {
    initCrcTable();

    if (flags & (EMIT_HDCP20_DESCRIPTOR | EMIT_HDCP21_DESCRIPTOR)) {
        int32_t hdcpVersion;
        if (flags & EMIT_HDCP20_DESCRIPTOR) {
            CHECK(!(flags & EMIT_HDCP21_DESCRIPTOR));
            hdcpVersion = 0x20;
        } else {
            CHECK(!(flags & EMIT_HDCP20_DESCRIPTOR));

            // HDCP2.0 _and_ HDCP 2.1 specs say to set the version
            // inside the HDCP descriptor to 0x20!!!
            hdcpVersion = 0x20;
        }

        // HDCP descriptor
        sp<ABuffer> descriptor = new ABuffer(7);
        uint8_t *data = descriptor->data();
        data[0] = 0x05;  // descriptor_tag
        data[1] = 5;  // descriptor_length
        data[2] = 'H';
        data[3] = 'D';
        data[4] = 'C';
        data[5] = 'P';
        data[6] = hdcpVersion;

        mProgramInfoDescriptors.push_back(descriptor);
    }
}

TSPacketizer::~TSPacketizer() {
}

ssize_t TSPacketizer::addTrack(const sp<AMessage> &format) {
    AString mime;
    CHECK(format->findString("mime", &mime));

    unsigned PIDStart;
    bool isVideo = !strncasecmp("video/", mime.c_str(), 6);
    bool isAudio = !strncasecmp("audio/", mime.c_str(), 6);

    if (isVideo) {
        PIDStart = 0x1011;
    } else if (isAudio) {
        PIDStart = 0x1100;
    } else {
        return ERROR_UNSUPPORTED;
    }

    unsigned streamType;
    unsigned streamIDStart;
    unsigned streamIDStop;

    if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_VIDEO_AVC)) {
        streamType = 0x1b;
        streamIDStart = 0xe0;
        streamIDStop = 0xef;
    } else if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        streamType = 0x0f;
        streamIDStart = 0xc0;
        streamIDStop = 0xdf;
    } else if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_RAW)) {
        streamType = 0x83;
        streamIDStart = 0xbd;
        streamIDStop = 0xbd;
    } else {
        return ERROR_UNSUPPORTED;
    }

    size_t numTracksOfThisType = 0;
    unsigned PID = PIDStart;

    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.itemAt(i);

        if (track->streamType() == streamType) {
            ++numTracksOfThisType;
        }

        if ((isAudio && track->isAudio()) || (isVideo && track->isVideo())) {
            ++PID;
        }
    }

    unsigned streamID = streamIDStart + numTracksOfThisType;
    if (streamID > streamIDStop) {
        return -ERANGE;
    }

    sp<Track> track = new Track(format, PID, streamType, streamID);
    return mTracks.add(track);
}

status_t TSPacketizer::extractCSDIfNecessary(size_t trackIndex) {
    if (trackIndex >= mTracks.size()) {
        return -ERANGE;
    }

    const sp<Track> &track = mTracks.itemAt(trackIndex);
    track->extractCSDIfNecessary();

    return OK;
}

status_t TSPacketizer::packetize(
        size_t trackIndex,
        const sp<ABuffer> &_accessUnit,
        sp<ABuffer> *packets,
        uint32_t flags,
        const uint8_t *PES_private_data, size_t PES_private_data_len,
        size_t numStuffingBytes) {
    sp<ABuffer> accessUnit = _accessUnit;

    int64_t timeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

    packets->clear();

    if (trackIndex >= mTracks.size()) {
        return -ERANGE;
    }

    const sp<Track> &track = mTracks.itemAt(trackIndex);

    if (track->isH264() && (flags & PREPEND_SPS_PPS_TO_IDR_FRAMES)
            && IsIDR(accessUnit)) {
        // prepend codec specific data, i.e. SPS and PPS.
        accessUnit = track->prependCSD(accessUnit);
    } else if (track->isAAC() && track->lacksADTSHeader()) {
        CHECK(!(flags & IS_ENCRYPTED));
        accessUnit = track->prependADTSHeader(accessUnit);
    }

    // 0x47
    // transport_error_indicator = b0
    // payload_unit_start_indicator = b1
    // transport_priority = b0
    // PID
    // transport_scrambling_control = b00
    // adaptation_field_control = b??
    // continuity_counter = b????
    // -- payload follows
    // packet_startcode_prefix = 0x000001
    // stream_id
    // PES_packet_length = 0x????
    // reserved = b10
    // PES_scrambling_control = b00
    // PES_priority = b0
    // data_alignment_indicator = b1
    // copyright = b0
    // original_or_copy = b0
    // PTS_DTS_flags = b10  (PTS only)
    // ESCR_flag = b0
    // ES_rate_flag = b0
    // DSM_trick_mode_flag = b0
    // additional_copy_info_flag = b0
    // PES_CRC_flag = b0
    // PES_extension_flag = b0
    // PES_header_data_length = 0x05
    // reserved = b0010 (PTS)
    // PTS[32..30] = b???
    // reserved = b1
    // PTS[29..15] = b??? ???? ???? ???? (15 bits)
    // reserved = b1
    // PTS[14..0] = b??? ???? ???? ???? (15 bits)
    // reserved = b1
    // the first fragment of "buffer" follows

    // Each transport packet (except for the last one contributing to the PES
    // payload) must contain a multiple of 16 bytes of payload per HDCP spec.
    bool alignPayload =
        (mFlags & (EMIT_HDCP20_DESCRIPTOR | EMIT_HDCP21_DESCRIPTOR));

    /*
       a) The very first PES transport stream packet contains

       4 bytes of TS header
       ... padding
       14 bytes of static PES header
       PES_private_data_len + 1 bytes (only if PES_private_data_len > 0)
       numStuffingBytes bytes

       followed by the payload

       b) Subsequent PES transport stream packets contain

       4 bytes of TS header
       ... padding

       followed by the payload
    */

    size_t PES_packet_length = accessUnit->size() + 8 + numStuffingBytes;
    if (PES_private_data_len > 0) {
        PES_packet_length += PES_private_data_len + 1;
    }

    size_t numTSPackets = 1;

    {
        // Make sure the PES header fits into a single TS packet:
        size_t PES_header_size = 14 + numStuffingBytes;
        if (PES_private_data_len > 0) {
            PES_header_size += PES_private_data_len + 1;
        }

        CHECK_LE(PES_header_size, 188u - 4u);

        size_t sizeAvailableForPayload = 188 - 4 - PES_header_size;
        size_t numBytesOfPayload = accessUnit->size();

        if (numBytesOfPayload > sizeAvailableForPayload) {
            numBytesOfPayload = sizeAvailableForPayload;

            if (alignPayload && numBytesOfPayload > 16) {
                numBytesOfPayload -= (numBytesOfPayload % 16);
            }
        }

        // size_t numPaddingBytes = sizeAvailableForPayload - numBytesOfPayload;
        ALOGV("packet 1 contains %zd padding bytes and %zd bytes of payload",
              numPaddingBytes, numBytesOfPayload);

        size_t numBytesOfPayloadRemaining = accessUnit->size() - numBytesOfPayload;

#if 0
        // The following hopefully illustrates the logic that led to the
        // more efficient computation in the #else block...

        while (numBytesOfPayloadRemaining > 0) {
            size_t sizeAvailableForPayload = 188 - 4;

            size_t numBytesOfPayload = numBytesOfPayloadRemaining;

            if (numBytesOfPayload > sizeAvailableForPayload) {
                numBytesOfPayload = sizeAvailableForPayload;

                if (alignPayload && numBytesOfPayload > 16) {
                    numBytesOfPayload -= (numBytesOfPayload % 16);
                }
            }

            size_t numPaddingBytes = sizeAvailableForPayload - numBytesOfPayload;
            ALOGI("packet %zd contains %zd padding bytes and %zd bytes of payload",
                    numTSPackets + 1, numPaddingBytes, numBytesOfPayload);

            numBytesOfPayloadRemaining -= numBytesOfPayload;
            ++numTSPackets;
        }
#else
        // This is how many bytes of payload each subsequent TS packet
        // can contain at most.
        sizeAvailableForPayload = 188 - 4;
        size_t sizeAvailableForAlignedPayload = sizeAvailableForPayload;
        if (alignPayload) {
            // We're only going to use a subset of the available space
            // since we need to make each fragment a multiple of 16 in size.
            sizeAvailableForAlignedPayload -=
                (sizeAvailableForAlignedPayload % 16);
        }

        size_t numFullTSPackets =
            numBytesOfPayloadRemaining / sizeAvailableForAlignedPayload;

        numTSPackets += numFullTSPackets;

        numBytesOfPayloadRemaining -=
            numFullTSPackets * sizeAvailableForAlignedPayload;

        // numBytesOfPayloadRemaining < sizeAvailableForAlignedPayload
        if (numFullTSPackets == 0 && numBytesOfPayloadRemaining > 0) {
            // There wasn't enough payload left to form a full aligned payload,
            // the last packet doesn't have to be aligned.
            ++numTSPackets;
        } else if (numFullTSPackets > 0
                && numBytesOfPayloadRemaining
                    + sizeAvailableForAlignedPayload > sizeAvailableForPayload) {
            // The last packet emitted had a full aligned payload and together
            // with the bytes remaining does exceed the unaligned payload
            // size, so we need another packet.
            ++numTSPackets;
        }
#endif
    }

    if (flags & EMIT_PAT_AND_PMT) {
        numTSPackets += 2;
    }

    if (flags & EMIT_PCR) {
        ++numTSPackets;
    }

    sp<ABuffer> buffer = new ABuffer(numTSPackets * 188);
    uint8_t *packetDataStart = buffer->data();

    if (flags & EMIT_PAT_AND_PMT) {
        // Program Association Table (PAT):
        // 0x47
        // transport_error_indicator = b0
        // payload_unit_start_indicator = b1
        // transport_priority = b0
        // PID = b0000000000000 (13 bits)
        // transport_scrambling_control = b00
        // adaptation_field_control = b01 (no adaptation field, payload only)
        // continuity_counter = b????
        // skip = 0x00
        // --- payload follows
        // table_id = 0x00
        // section_syntax_indicator = b1
        // must_be_zero = b0
        // reserved = b11
        // section_length = 0x00d
        // transport_stream_id = 0x0000
        // reserved = b11
        // version_number = b00001
        // current_next_indicator = b1
        // section_number = 0x00
        // last_section_number = 0x00
        //   one program follows:
        //   program_number = 0x0001
        //   reserved = b111
        //   program_map_PID = kPID_PMT (13 bits!)
        // CRC = 0x????????

        if (++mPATContinuityCounter == 16) {
            mPATContinuityCounter = 0;
        }

        uint8_t *ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x40;
        *ptr++ = 0x00;
        *ptr++ = 0x10 | mPATContinuityCounter;
        *ptr++ = 0x00;

        uint8_t *crcDataStart = ptr;
        *ptr++ = 0x00;
        *ptr++ = 0xb0;
        *ptr++ = 0x0d;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0xc3;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0x01;
        *ptr++ = 0xe0 | (kPID_PMT >> 8);
        *ptr++ = kPID_PMT & 0xff;

        CHECK_EQ(ptr - crcDataStart, 12);
        uint32_t crc = htonl(crc32(crcDataStart, ptr - crcDataStart));
        memcpy(ptr, &crc, 4);
        ptr += 4;

        size_t sizeLeft = packetDataStart + 188 - ptr;
        memset(ptr, 0xff, sizeLeft);

        packetDataStart += 188;

        // Program Map (PMT):
        // 0x47
        // transport_error_indicator = b0
        // payload_unit_start_indicator = b1
        // transport_priority = b0
        // PID = kPID_PMT (13 bits)
        // transport_scrambling_control = b00
        // adaptation_field_control = b01 (no adaptation field, payload only)
        // continuity_counter = b????
        // skip = 0x00
        // -- payload follows
        // table_id = 0x02
        // section_syntax_indicator = b1
        // must_be_zero = b0
        // reserved = b11
        // section_length = 0x???
        // program_number = 0x0001
        // reserved = b11
        // version_number = b00001
        // current_next_indicator = b1
        // section_number = 0x00
        // last_section_number = 0x00
        // reserved = b111
        // PCR_PID = kPCR_PID (13 bits)
        // reserved = b1111
        // program_info_length = 0x???
        //   program_info_descriptors follow
        // one or more elementary stream descriptions follow:
        //   stream_type = 0x??
        //   reserved = b111
        //   elementary_PID = b? ???? ???? ???? (13 bits)
        //   reserved = b1111
        //   ES_info_length = 0x000
        // CRC = 0x????????

        if (++mPMTContinuityCounter == 16) {
            mPMTContinuityCounter = 0;
        }

        ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x40 | (kPID_PMT >> 8);
        *ptr++ = kPID_PMT & 0xff;
        *ptr++ = 0x10 | mPMTContinuityCounter;
        *ptr++ = 0x00;

        crcDataStart = ptr;
        *ptr++ = 0x02;

        *ptr++ = 0x00;  // section_length to be filled in below.
        *ptr++ = 0x00;

        *ptr++ = 0x00;
        *ptr++ = 0x01;
        *ptr++ = 0xc3;
        *ptr++ = 0x00;
        *ptr++ = 0x00;
        *ptr++ = 0xe0 | (kPID_PCR >> 8);
        *ptr++ = kPID_PCR & 0xff;

        size_t program_info_length = 0;
        for (size_t i = 0; i < mProgramInfoDescriptors.size(); ++i) {
            program_info_length += mProgramInfoDescriptors.itemAt(i)->size();
        }

        CHECK_LT(program_info_length, 0x400);
        *ptr++ = 0xf0 | (program_info_length >> 8);
        *ptr++ = (program_info_length & 0xff);

        for (size_t i = 0; i < mProgramInfoDescriptors.size(); ++i) {
            const sp<ABuffer> &desc = mProgramInfoDescriptors.itemAt(i);
            memcpy(ptr, desc->data(), desc->size());
            ptr += desc->size();
        }

        for (size_t i = 0; i < mTracks.size(); ++i) {
            const sp<Track> &track = mTracks.itemAt(i);

            // Make sure all the decriptors have been added.
            track->finalize();

            *ptr++ = track->streamType();
            *ptr++ = 0xe0 | (track->PID() >> 8);
            *ptr++ = track->PID() & 0xff;

            size_t ES_info_length = 0;
            for (size_t i = 0; i < track->countDescriptors(); ++i) {
                ES_info_length += track->descriptorAt(i)->size();
            }
            CHECK_LE(ES_info_length, 0xfff);

            *ptr++ = 0xf0 | (ES_info_length >> 8);
            *ptr++ = (ES_info_length & 0xff);

            for (size_t i = 0; i < track->countDescriptors(); ++i) {
                const sp<ABuffer> &descriptor = track->descriptorAt(i);
                memcpy(ptr, descriptor->data(), descriptor->size());
                ptr += descriptor->size();
            }
        }

        size_t section_length = ptr - (crcDataStart + 3) + 4 /* CRC */;

        crcDataStart[1] = 0xb0 | (section_length >> 8);
        crcDataStart[2] = section_length & 0xff;

        crc = htonl(crc32(crcDataStart, ptr - crcDataStart));
        memcpy(ptr, &crc, 4);
        ptr += 4;

        sizeLeft = packetDataStart + 188 - ptr;
        memset(ptr, 0xff, sizeLeft);

        packetDataStart += 188;
    }

    if (flags & EMIT_PCR) {
        // PCR stream
        // 0x47
        // transport_error_indicator = b0
        // payload_unit_start_indicator = b1
        // transport_priority = b0
        // PID = kPCR_PID (13 bits)
        // transport_scrambling_control = b00
        // adaptation_field_control = b10 (adaptation field only, no payload)
        // continuity_counter = b0000 (does not increment)
        // adaptation_field_length = 183
        // discontinuity_indicator = b0
        // random_access_indicator = b0
        // elementary_stream_priority_indicator = b0
        // PCR_flag = b1
        // OPCR_flag = b0
        // splicing_point_flag = b0
        // transport_private_data_flag = b0
        // adaptation_field_extension_flag = b0
        // program_clock_reference_base = b?????????????????????????????????
        // reserved = b111111
        // program_clock_reference_extension = b?????????

        int64_t nowUs = ALooper::GetNowUs();

        uint64_t PCR = nowUs * 27;  // PCR based on a 27MHz clock
        uint64_t PCR_base = PCR / 300;
        uint32_t PCR_ext = PCR % 300;

        uint8_t *ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x40 | (kPID_PCR >> 8);
        *ptr++ = kPID_PCR & 0xff;
        *ptr++ = 0x20;
        *ptr++ = 0xb7;  // adaptation_field_length
        *ptr++ = 0x10;
        *ptr++ = (PCR_base >> 25) & 0xff;
        *ptr++ = (PCR_base >> 17) & 0xff;
        *ptr++ = (PCR_base >> 9) & 0xff;
        *ptr++ = ((PCR_base & 1) << 7) | 0x7e | ((PCR_ext >> 8) & 1);
        *ptr++ = (PCR_ext & 0xff);

        size_t sizeLeft = packetDataStart + 188 - ptr;
        memset(ptr, 0xff, sizeLeft);

        packetDataStart += 188;
    }

    uint64_t PTS = (timeUs * 9ll) / 100ll;

    if (PES_packet_length >= 65536) {
        // This really should only happen for video.
        CHECK(track->isVideo());

        // It's valid to set this to 0 for video according to the specs.
        PES_packet_length = 0;
    }

    size_t sizeAvailableForPayload = 188 - 4 - 14 - numStuffingBytes;
    if (PES_private_data_len > 0) {
        sizeAvailableForPayload -= PES_private_data_len + 1;
    }

    size_t copy = accessUnit->size();

    if (copy > sizeAvailableForPayload) {
        copy = sizeAvailableForPayload;

        if (alignPayload && copy > 16) {
            copy -= (copy % 16);
        }
    }

    size_t numPaddingBytes = sizeAvailableForPayload - copy;

    uint8_t *ptr = packetDataStart;
    *ptr++ = 0x47;
    *ptr++ = 0x40 | (track->PID() >> 8);
    *ptr++ = track->PID() & 0xff;

    *ptr++ = (numPaddingBytes > 0 ? 0x30 : 0x10)
                | track->incrementContinuityCounter();

    if (numPaddingBytes > 0) {
        *ptr++ = numPaddingBytes - 1;
        if (numPaddingBytes >= 2) {
            *ptr++ = 0x00;
            memset(ptr, 0xff, numPaddingBytes - 2);
            ptr += numPaddingBytes - 2;
        }
    }

    *ptr++ = 0x00;
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    *ptr++ = track->streamID();
    *ptr++ = PES_packet_length >> 8;
    *ptr++ = PES_packet_length & 0xff;
    *ptr++ = 0x84;
    *ptr++ = (PES_private_data_len > 0) ? 0x81 : 0x80;

    size_t headerLength = 0x05 + numStuffingBytes;
    if (PES_private_data_len > 0) {
        headerLength += 1 + PES_private_data_len;
    }

    *ptr++ = headerLength;

    *ptr++ = 0x20 | (((PTS >> 30) & 7) << 1) | 1;
    *ptr++ = (PTS >> 22) & 0xff;
    *ptr++ = (((PTS >> 15) & 0x7f) << 1) | 1;
    *ptr++ = (PTS >> 7) & 0xff;
    *ptr++ = ((PTS & 0x7f) << 1) | 1;

    if (PES_private_data_len > 0) {
        *ptr++ = 0x8e;  // PES_private_data_flag, reserved.
        memcpy(ptr, PES_private_data, PES_private_data_len);
        ptr += PES_private_data_len;
    }

    for (size_t i = 0; i < numStuffingBytes; ++i) {
        *ptr++ = 0xff;
    }

    memcpy(ptr, accessUnit->data(), copy);
    ptr += copy;

    CHECK_EQ(ptr, packetDataStart + 188);
    packetDataStart += 188;

    size_t offset = copy;
    while (offset < accessUnit->size()) {
        // for subsequent fragments of "buffer":
        // 0x47
        // transport_error_indicator = b0
        // payload_unit_start_indicator = b0
        // transport_priority = b0
        // PID = b0 0001 1110 ???? (13 bits) [0x1e0 + 1 + sourceIndex]
        // transport_scrambling_control = b00
        // adaptation_field_control = b??
        // continuity_counter = b????
        // the fragment of "buffer" follows.

        size_t sizeAvailableForPayload = 188 - 4;

        size_t copy = accessUnit->size() - offset;

        if (copy > sizeAvailableForPayload) {
            copy = sizeAvailableForPayload;

            if (alignPayload && copy > 16) {
                copy -= (copy % 16);
            }
        }

        size_t numPaddingBytes = sizeAvailableForPayload - copy;

        uint8_t *ptr = packetDataStart;
        *ptr++ = 0x47;
        *ptr++ = 0x00 | (track->PID() >> 8);
        *ptr++ = track->PID() & 0xff;

        *ptr++ = (numPaddingBytes > 0 ? 0x30 : 0x10)
                    | track->incrementContinuityCounter();

        if (numPaddingBytes > 0) {
            *ptr++ = numPaddingBytes - 1;
            if (numPaddingBytes >= 2) {
                *ptr++ = 0x00;
                memset(ptr, 0xff, numPaddingBytes - 2);
                ptr += numPaddingBytes - 2;
            }
        }

        memcpy(ptr, accessUnit->data() + offset, copy);
        ptr += copy;
        CHECK_EQ(ptr, packetDataStart + 188);

        offset += copy;
        packetDataStart += 188;
    }

    CHECK(packetDataStart == buffer->data() + buffer->capacity());

    *packets = buffer;

    return OK;
}

void TSPacketizer::initCrcTable() {
    uint32_t poly = 0x04C11DB7;

    for (int i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            crc = (crc << 1) ^ ((crc & 0x80000000) ? (poly) : 0);
        }
        mCrcTable[i] = crc;
    }
}

uint32_t TSPacketizer::crc32(const uint8_t *start, size_t size) const {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p;

    for (p = start; p < start + size; ++p) {
        crc = (crc << 8) ^ mCrcTable[((crc >> 24) ^ *p) & 0xFF];
    }

    return crc;
}

sp<ABuffer> TSPacketizer::prependCSD(
        size_t trackIndex, const sp<ABuffer> &accessUnit) const {
    CHECK_LT(trackIndex, mTracks.size());

    const sp<Track> &track = mTracks.itemAt(trackIndex);
    CHECK(track->isH264() && IsIDR(accessUnit));

    int64_t timeUs;
    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

    sp<ABuffer> accessUnit2 = track->prependCSD(accessUnit);

    accessUnit2->meta()->setInt64("timeUs", timeUs);

    return accessUnit2;
}

}  // namespace android

