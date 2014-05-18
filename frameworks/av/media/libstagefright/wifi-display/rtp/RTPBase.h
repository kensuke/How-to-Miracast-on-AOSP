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

#ifndef RTP_BASE_H_

#define RTP_BASE_H_

namespace android {

struct RTPBase {
    enum PacketizationMode {
        PACKETIZATION_TRANSPORT_STREAM,
        PACKETIZATION_H264,
        PACKETIZATION_AAC,
        PACKETIZATION_NONE,
    };

    enum TransportMode {
        TRANSPORT_UNDEFINED,
        TRANSPORT_NONE,
        TRANSPORT_UDP,
        TRANSPORT_TCP,
        TRANSPORT_TCP_INTERLEAVED,
    };

    enum {
        // Really UDP _payload_ size
        kMaxUDPPacketSize = 1472,   // 1472 good, 1473 bad on Android@Home
    };

    static int32_t PickRandomRTPPort();
};

}  // namespace android

#endif  // RTP_BASE_H_


