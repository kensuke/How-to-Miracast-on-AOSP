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

//#define LOG_NEBUG 0
#define LOG_TAG "udptest"
#include <utils/Log.h>

#include "TimeSyncer.h"

#include <binder/ProcessState.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>

namespace android {

}  // namespace android

static void usage(const char *me) {
    fprintf(stderr,
            "usage: %s -c host[:port]\tconnect to test server\n"
            "           -l            \tcreate a test server\n",
            me);
}

int main(int argc, char **argv) {
    using namespace android;

    ProcessState::self()->startThreadPool();

    int32_t localPort = -1;
    int32_t connectToPort = -1;
    AString connectToHost;

    int res;
    while ((res = getopt(argc, argv, "hc:l:")) >= 0) {
        switch (res) {
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    connectToHost = optarg;
                    connectToPort = 49152;
                } else {
                    connectToHost.setTo(optarg, colonPos - optarg);

                    char *end;
                    connectToPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || connectToPort < 1 || connectToPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }

            case 'l':
            {
                char *end;
                localPort = strtol(optarg, &end, 10);

                if (*end != '\0' || end == optarg
                        || localPort < 1 || localPort > 65535) {
                    fprintf(stderr, "Illegal port specified.\n");
                    exit(1);
                }
                break;
            }

            case '?':
            case 'h':
                usage(argv[0]);
                exit(1);
        }
    }

    if (localPort < 0 && connectToPort < 0) {
        fprintf(stderr,
                "You need to select either client or server mode.\n");
        exit(1);
    }

    sp<ANetworkSession> netSession = new ANetworkSession;
    netSession->start();

    sp<ALooper> looper = new ALooper;

    sp<TimeSyncer> handler = new TimeSyncer(netSession, NULL /* notify */);
    looper->registerHandler(handler);

    if (localPort >= 0) {
        handler->startServer(localPort);
    } else {
        handler->startClient(connectToHost.c_str(), connectToPort);
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}

