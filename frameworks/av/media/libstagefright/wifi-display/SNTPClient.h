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

#ifndef SNTP_CLIENT_H_

#define SNTP_CLIENT_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Errors.h>

namespace android {

// Implementation of the SNTP (Simple Network Time Protocol)
struct SNTPClient {
    SNTPClient();

    status_t requestTime(const char *host);

    // given a time obtained from ALooper::GetNowUs()
    // return the number of us elapsed since Jan 1 1970 00:00:00 (UTC).
    int64_t adjustTimeUs(int64_t timeUs) const;

private:
    enum {
        kNTPPort = 123,
        kNTPPacketSize = 48,
        kNTPModeClient = 3,
        kNTPVersion = 3,
        kNTPTransmitTimeOffset = 40,
        kNTPOriginateTimeOffset = 24,
        kNTPReceiveTimeOffset = 32,
    };

    uint64_t mTimeReferenceNTP;
    int64_t mTimeReferenceUs;
    int64_t mRoundTripTimeNTP;

    static void writeTimeStamp(uint8_t *dst, uint64_t ntpTime);
    static uint64_t readTimeStamp(const uint8_t *dst);

    static uint64_t getNowNTP();
    static uint64_t makeNTP(uint64_t deltaUs);

    DISALLOW_EVIL_CONSTRUCTORS(SNTPClient);
};

}  // namespace android

#endif  // SNTP_CLIENT_H_
