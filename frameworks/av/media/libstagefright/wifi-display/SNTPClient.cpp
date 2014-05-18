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

#include "SNTPClient.h"

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/Utils.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace android {

SNTPClient::SNTPClient() {
}

status_t SNTPClient::requestTime(const char *host) {
    struct hostent *ent;
    int64_t requestTimeNTP, requestTimeUs;
    ssize_t n;
    int64_t responseTimeUs, responseTimeNTP;
    int64_t originateTimeNTP, receiveTimeNTP, transmitTimeNTP;
    int64_t roundTripTimeNTP, clockOffsetNTP;

    status_t err = UNKNOWN_ERROR;

    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) {
        err = -errno;

        goto bail;
    }

    ent = gethostbyname(host);

    if (ent == NULL) {
        err = -ENOENT;
        goto bail2;
    }

    struct sockaddr_in hostAddr;
    memset(hostAddr.sin_zero, 0, sizeof(hostAddr.sin_zero));
    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons(kNTPPort);
    hostAddr.sin_addr.s_addr = *(in_addr_t *)ent->h_addr;

    uint8_t packet[kNTPPacketSize];
    memset(packet, 0, sizeof(packet));

    packet[0] = kNTPModeClient | (kNTPVersion << 3);

    requestTimeNTP = getNowNTP();
    requestTimeUs = ALooper::GetNowUs();
    writeTimeStamp(&packet[kNTPTransmitTimeOffset], requestTimeNTP);

    n = sendto(
            s, packet, sizeof(packet), 0,
            (const struct sockaddr *)&hostAddr, sizeof(hostAddr));

    if (n < 0) {
        err = -errno;
        goto bail2;
    }

    memset(packet, 0, sizeof(packet));

    do {
        n = recv(s, packet, sizeof(packet), 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        err = -errno;
        goto bail2;
    }

    responseTimeUs = ALooper::GetNowUs();

    responseTimeNTP = requestTimeNTP + makeNTP(responseTimeUs - requestTimeUs);

    originateTimeNTP = readTimeStamp(&packet[kNTPOriginateTimeOffset]);
    receiveTimeNTP = readTimeStamp(&packet[kNTPReceiveTimeOffset]);
    transmitTimeNTP = readTimeStamp(&packet[kNTPTransmitTimeOffset]);

    roundTripTimeNTP =
        makeNTP(responseTimeUs - requestTimeUs)
            - (transmitTimeNTP - receiveTimeNTP);

    clockOffsetNTP =
        ((receiveTimeNTP - originateTimeNTP)
            + (transmitTimeNTP - responseTimeNTP)) / 2;

    mTimeReferenceNTP = responseTimeNTP + clockOffsetNTP;
    mTimeReferenceUs = responseTimeUs;
    mRoundTripTimeNTP = roundTripTimeNTP;

    err = OK;

bail2:
    close(s);
    s = -1;

bail:
    return err;
}

int64_t SNTPClient::adjustTimeUs(int64_t timeUs) const {
    uint64_t nowNTP =
        mTimeReferenceNTP + makeNTP(timeUs - mTimeReferenceUs);

    int64_t nowUs =
        (nowNTP >> 32) * 1000000ll
        + ((nowNTP & 0xffffffff) * 1000000ll) / (1ll << 32);

    nowUs -= ((70ll * 365 + 17) * 24) * 60 * 60 * 1000000ll;

    return nowUs;
}

// static
void SNTPClient::writeTimeStamp(uint8_t *dst, uint64_t ntpTime) {
    *dst++ = (ntpTime >> 56) & 0xff;
    *dst++ = (ntpTime >> 48) & 0xff;
    *dst++ = (ntpTime >> 40) & 0xff;
    *dst++ = (ntpTime >> 32) & 0xff;
    *dst++ = (ntpTime >> 24) & 0xff;
    *dst++ = (ntpTime >> 16) & 0xff;
    *dst++ = (ntpTime >> 8) & 0xff;
    *dst++ = ntpTime & 0xff;
}

// static
uint64_t SNTPClient::readTimeStamp(const uint8_t *dst) {
    return U64_AT(dst);
}

// static
uint64_t SNTPClient::getNowNTP() {
    struct timeval tv;
    gettimeofday(&tv, NULL /* time zone */);

    uint64_t nowUs = tv.tv_sec * 1000000ll + tv.tv_usec;

    nowUs += ((70ll * 365 + 17) * 24) * 60 * 60 * 1000000ll;

    return makeNTP(nowUs);
}

// static
uint64_t SNTPClient::makeNTP(uint64_t deltaUs) {
    uint64_t hi = deltaUs / 1000000ll;
    uint64_t lo = ((1ll << 32) * (deltaUs % 1000000ll)) / 1000000ll;

    return (hi << 32) | lo;
}

}  // namespace android

