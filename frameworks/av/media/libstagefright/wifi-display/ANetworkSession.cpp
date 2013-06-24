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
#define LOG_TAG "NetworkSession"
#include <utils/Log.h>

#include "ANetworkSession.h"
#include "ParsedMessage.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/Utils.h>

namespace android {

static const size_t kMaxUDPSize = 1500;

struct ANetworkSession::NetworkThread : public Thread {
    NetworkThread(ANetworkSession *session);

protected:
    virtual ~NetworkThread();

private:
    ANetworkSession *mSession;

    virtual bool threadLoop();

    DISALLOW_EVIL_CONSTRUCTORS(NetworkThread);
};

struct ANetworkSession::Session : public RefBase {
    enum State {
        CONNECTING,
        CONNECTED,
        LISTENING_RTSP,
        LISTENING_TCP_DGRAMS,
        DATAGRAM,
    };

    Session(int32_t sessionID,
            State state,
            int s,
            const sp<AMessage> &notify);

    int32_t sessionID() const;
    int socket() const;
    sp<AMessage> getNotificationMessage() const;

    bool isRTSPServer() const;
    bool isTCPDatagramServer() const;

    bool wantsToRead();
    bool wantsToWrite();

    status_t readMore();
    status_t writeMore();

    status_t sendRequest(const void *data, ssize_t size);

    void setIsRTSPConnection(bool yesno);

protected:
    virtual ~Session();

private:
    int32_t mSessionID;
    State mState;
    bool mIsRTSPConnection;
    int mSocket;
    sp<AMessage> mNotify;
    bool mSawReceiveFailure, mSawSendFailure;

    // for TCP / stream data
    AString mOutBuffer;

    // for UDP / datagrams
    List<sp<ABuffer> > mOutDatagrams;

    AString mInBuffer;

    void notifyError(bool send, status_t err, const char *detail);
    void notify(NotificationReason reason);

    DISALLOW_EVIL_CONSTRUCTORS(Session);
};
////////////////////////////////////////////////////////////////////////////////

ANetworkSession::NetworkThread::NetworkThread(ANetworkSession *session)
    : mSession(session) {
}

ANetworkSession::NetworkThread::~NetworkThread() {
}

bool ANetworkSession::NetworkThread::threadLoop() {
    mSession->threadLoop();

    return true;
}

////////////////////////////////////////////////////////////////////////////////

ANetworkSession::Session::Session(
        int32_t sessionID,
        State state,
        int s,
        const sp<AMessage> &notify)
    : mSessionID(sessionID),
      mState(state),
      mIsRTSPConnection(false),
      mSocket(s),
      mNotify(notify),
      mSawReceiveFailure(false),
      mSawSendFailure(false) {
    if (mState == CONNECTED) {
        struct sockaddr_in localAddr;
        socklen_t localAddrLen = sizeof(localAddr);

        int res = getsockname(
                mSocket, (struct sockaddr *)&localAddr, &localAddrLen);
        CHECK_GE(res, 0);

        struct sockaddr_in remoteAddr;
        socklen_t remoteAddrLen = sizeof(remoteAddr);

        res = getpeername(
                mSocket, (struct sockaddr *)&remoteAddr, &remoteAddrLen);
        CHECK_GE(res, 0);

        in_addr_t addr = ntohl(localAddr.sin_addr.s_addr);
        AString localAddrString = StringPrintf(
                "%d.%d.%d.%d",
                (addr >> 24),
                (addr >> 16) & 0xff,
                (addr >> 8) & 0xff,
                addr & 0xff);

        addr = ntohl(remoteAddr.sin_addr.s_addr);
        AString remoteAddrString = StringPrintf(
                "%d.%d.%d.%d",
                (addr >> 24),
                (addr >> 16) & 0xff,
                (addr >> 8) & 0xff,
                addr & 0xff);

        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("sessionID", mSessionID);
        msg->setInt32("reason", kWhatClientConnected);
        msg->setString("server-ip", localAddrString.c_str());
        msg->setInt32("server-port", ntohs(localAddr.sin_port));
        msg->setString("client-ip", remoteAddrString.c_str());
        msg->setInt32("client-port", ntohs(remoteAddr.sin_port));
        msg->post();
    }
}

ANetworkSession::Session::~Session() {
    ALOGV("Session %d gone", mSessionID);

    close(mSocket);
    mSocket = -1;
}

int32_t ANetworkSession::Session::sessionID() const {
    return mSessionID;
}

int ANetworkSession::Session::socket() const {
    return mSocket;
}

void ANetworkSession::Session::setIsRTSPConnection(bool yesno) {
    mIsRTSPConnection = yesno;
}

sp<AMessage> ANetworkSession::Session::getNotificationMessage() const {
    return mNotify;
}

bool ANetworkSession::Session::isRTSPServer() const {
    return mState == LISTENING_RTSP;
}

bool ANetworkSession::Session::isTCPDatagramServer() const {
    return mState == LISTENING_TCP_DGRAMS;
}

bool ANetworkSession::Session::wantsToRead() {
    return !mSawReceiveFailure && mState != CONNECTING;
}

bool ANetworkSession::Session::wantsToWrite() {
    return !mSawSendFailure
        && (mState == CONNECTING
            || (mState == CONNECTED && !mOutBuffer.empty())
            || (mState == DATAGRAM && !mOutDatagrams.empty()));
}

status_t ANetworkSession::Session::readMore() {
    if (mState == DATAGRAM) {
        status_t err;
        do {
            sp<ABuffer> buf = new ABuffer(kMaxUDPSize);

            struct sockaddr_in remoteAddr;
            socklen_t remoteAddrLen = sizeof(remoteAddr);

            ssize_t n;
            do {
                n = recvfrom(
                        mSocket, buf->data(), buf->capacity(), 0,
                        (struct sockaddr *)&remoteAddr, &remoteAddrLen);
            } while (n < 0 && errno == EINTR);

            err = OK;
            if (n < 0) {
                err = -errno;
            } else if (n == 0) {
                err = -ECONNRESET;
            } else {
                buf->setRange(0, n);

                int64_t nowUs = ALooper::GetNowUs();
                buf->meta()->setInt64("arrivalTimeUs", nowUs);

                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("sessionID", mSessionID);
                notify->setInt32("reason", kWhatDatagram);

                uint32_t ip = ntohl(remoteAddr.sin_addr.s_addr);
                notify->setString(
                        "fromAddr",
                        StringPrintf(
                            "%u.%u.%u.%u",
                            ip >> 24,
                            (ip >> 16) & 0xff,
                            (ip >> 8) & 0xff,
                            ip & 0xff).c_str());

                notify->setInt32("fromPort", ntohs(remoteAddr.sin_port));

                notify->setBuffer("data", buf);
                notify->post();
            }
        } while (err == OK);

        if (err == -EAGAIN) {
            err = OK;
        }

        if (err != OK) {
            notifyError(false /* send */, err, "Recvfrom failed.");
            mSawReceiveFailure = true;
        }

        return err;
    }

    char tmp[512];
    ssize_t n;
    do {
        n = recv(mSocket, tmp, sizeof(tmp), 0);
    } while (n < 0 && errno == EINTR);

    status_t err = OK;

    if (n > 0) {
        mInBuffer.append(tmp, n);

#if 0
        ALOGI("in:");
        hexdump(tmp, n);
#endif
    } else if (n < 0) {
        err = -errno;
    } else {
        err = -ECONNRESET;
    }

    if (!mIsRTSPConnection) {
        // TCP stream carrying 16-bit length-prefixed datagrams.

        while (mInBuffer.size() >= 2) {
            size_t packetSize = U16_AT((const uint8_t *)mInBuffer.c_str());

            if (mInBuffer.size() < packetSize + 2) {
                break;
            }

            sp<ABuffer> packet = new ABuffer(packetSize);
            memcpy(packet->data(), mInBuffer.c_str() + 2, packetSize);

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("sessionID", mSessionID);
            notify->setInt32("reason", kWhatDatagram);
            notify->setBuffer("data", packet);
            notify->post();

            mInBuffer.erase(0, packetSize + 2);
        }
    } else {
        for (;;) {
            size_t length;

            if (mInBuffer.size() > 0 && mInBuffer.c_str()[0] == '$') {
                if (mInBuffer.size() < 4) {
                    break;
                }

                length = U16_AT((const uint8_t *)mInBuffer.c_str() + 2);

                if (mInBuffer.size() < 4 + length) {
                    break;
                }

                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("sessionID", mSessionID);
                notify->setInt32("reason", kWhatBinaryData);
                notify->setInt32("channel", mInBuffer.c_str()[1]);

                sp<ABuffer> data = new ABuffer(length);
                memcpy(data->data(), mInBuffer.c_str() + 4, length);

                int64_t nowUs = ALooper::GetNowUs();
                data->meta()->setInt64("arrivalTimeUs", nowUs);

                notify->setBuffer("data", data);
                notify->post();

                mInBuffer.erase(0, 4 + length);
                continue;
            }

            sp<ParsedMessage> msg =
                ParsedMessage::Parse(
                        mInBuffer.c_str(), mInBuffer.size(), err != OK, &length);

            if (msg == NULL) {
                break;
            }

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("sessionID", mSessionID);
            notify->setInt32("reason", kWhatData);
            notify->setObject("data", msg);
            notify->post();

#if 1
            // XXX The (old) dongle sends the wrong content length header on a
            // SET_PARAMETER request that signals a "wfd_idr_request".
            // (17 instead of 19).
            const char *content = msg->getContent();
            if (content
                    && !memcmp(content, "wfd_idr_request\r\n", 17)
                    && length >= 19
                    && mInBuffer.c_str()[length] == '\r'
                    && mInBuffer.c_str()[length + 1] == '\n') {
                length += 2;
            }
#endif

            mInBuffer.erase(0, length);

            if (err != OK) {
                break;
            }
        }
    }

    if (err != OK) {
        notifyError(false /* send */, err, "Recv failed.");
        mSawReceiveFailure = true;
    }

    return err;
}

status_t ANetworkSession::Session::writeMore() {
    if (mState == DATAGRAM) {
        CHECK(!mOutDatagrams.empty());

        status_t err;
        do {
            const sp<ABuffer> &datagram = *mOutDatagrams.begin();

            uint8_t *data = datagram->data();
            if (data[0] == 0x80 && (data[1] & 0x7f) == 33) {
                int64_t nowUs = ALooper::GetNowUs();

                uint32_t prevRtpTime = U32_AT(&data[4]);

                // 90kHz time scale
                uint32_t rtpTime = (nowUs * 9ll) / 100ll;
                int32_t diffTime = (int32_t)rtpTime - (int32_t)prevRtpTime;

                ALOGV("correcting rtpTime by %.0f ms", diffTime / 90.0);

                data[4] = rtpTime >> 24;
                data[5] = (rtpTime >> 16) & 0xff;
                data[6] = (rtpTime >> 8) & 0xff;
                data[7] = rtpTime & 0xff;
            }

            int n;
            do {
                n = send(mSocket, datagram->data(), datagram->size(), 0);
            } while (n < 0 && errno == EINTR);

            err = OK;

            if (n > 0) {
                mOutDatagrams.erase(mOutDatagrams.begin());
            } else if (n < 0) {
                err = -errno;
            } else if (n == 0) {
                err = -ECONNRESET;
            }
        } while (err == OK && !mOutDatagrams.empty());

        if (err == -EAGAIN) {
            if (!mOutDatagrams.empty()) {
                ALOGI("%d datagrams remain queued.", mOutDatagrams.size());
            }
            err = OK;
        }

        if (err != OK) {
            notifyError(true /* send */, err, "Send datagram failed.");
            mSawSendFailure = true;
        }

        return err;
    }

    if (mState == CONNECTING) {
        int err;
        socklen_t optionLen = sizeof(err);
        CHECK_EQ(getsockopt(mSocket, SOL_SOCKET, SO_ERROR, &err, &optionLen), 0);
        CHECK_EQ(optionLen, (socklen_t)sizeof(err));

        if (err != 0) {
            notifyError(kWhatError, -err, "Connection failed");
            mSawSendFailure = true;

            return -err;
        }

        mState = CONNECTED;
        notify(kWhatConnected);

        return OK;
    }

    CHECK_EQ(mState, CONNECTED);
    CHECK(!mOutBuffer.empty());

    ssize_t n;
    do {
        n = send(mSocket, mOutBuffer.c_str(), mOutBuffer.size(), 0);
    } while (n < 0 && errno == EINTR);

    status_t err = OK;

    if (n > 0) {
#if 0
        ALOGI("out:");
        hexdump(mOutBuffer.c_str(), n);
#endif

        mOutBuffer.erase(0, n);
    } else if (n < 0) {
        err = -errno;
    } else if (n == 0) {
        err = -ECONNRESET;
    }

    if (err != OK) {
        notifyError(true /* send */, err, "Send failed.");
        mSawSendFailure = true;
    }

    return err;
}

status_t ANetworkSession::Session::sendRequest(const void *data, ssize_t size) {
    CHECK(mState == CONNECTED || mState == DATAGRAM);

    if (mState == DATAGRAM) {
        CHECK_GE(size, 0);

        sp<ABuffer> datagram = new ABuffer(size);
        memcpy(datagram->data(), data, size);

        mOutDatagrams.push_back(datagram);
        return OK;
    }

    if (mState == CONNECTED && !mIsRTSPConnection) {
        CHECK_LE(size, 65535);

        uint8_t prefix[2];
        prefix[0] = size >> 8;
        prefix[1] = size & 0xff;

        mOutBuffer.append((const char *)prefix, sizeof(prefix));
    }

    mOutBuffer.append(
            (const char *)data,
            (size >= 0) ? size : strlen((const char *)data));

    return OK;
}

void ANetworkSession::Session::notifyError(
        bool send, status_t err, const char *detail) {
    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("sessionID", mSessionID);
    msg->setInt32("reason", kWhatError);
    msg->setInt32("send", send);
    msg->setInt32("err", err);
    msg->setString("detail", detail);
    msg->post();
}

void ANetworkSession::Session::notify(NotificationReason reason) {
    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("sessionID", mSessionID);
    msg->setInt32("reason", reason);
    msg->post();
}

////////////////////////////////////////////////////////////////////////////////

ANetworkSession::ANetworkSession()
    : mNextSessionID(1) {
    mPipeFd[0] = mPipeFd[1] = -1;
}

ANetworkSession::~ANetworkSession() {
    stop();
}

status_t ANetworkSession::start() {
    if (mThread != NULL) {
        return INVALID_OPERATION;
    }

    int res = pipe(mPipeFd);
    if (res != 0) {
        mPipeFd[0] = mPipeFd[1] = -1;
        return -errno;
    }

    mThread = new NetworkThread(this);

    status_t err = mThread->run("ANetworkSession", ANDROID_PRIORITY_AUDIO);

    if (err != OK) {
        mThread.clear();

        close(mPipeFd[0]);
        close(mPipeFd[1]);
        mPipeFd[0] = mPipeFd[1] = -1;

        return err;
    }

    return OK;
}

status_t ANetworkSession::stop() {
    if (mThread == NULL) {
        return INVALID_OPERATION;
    }

    mThread->requestExit();
    interrupt();
    mThread->requestExitAndWait();

    mThread.clear();

    close(mPipeFd[0]);
    close(mPipeFd[1]);
    mPipeFd[0] = mPipeFd[1] = -1;

    return OK;
}

status_t ANetworkSession::createRTSPClient(
        const char *host, unsigned port, const sp<AMessage> &notify,
        int32_t *sessionID) {
    return createClientOrServer(
            kModeCreateRTSPClient,
            NULL /* addr */,
            0 /* port */,
            host,
            port,
            notify,
            sessionID);
}

status_t ANetworkSession::createRTSPServer(
        const struct in_addr &addr, unsigned port,
        const sp<AMessage> &notify, int32_t *sessionID) {
    return createClientOrServer(
            kModeCreateRTSPServer,
            &addr,
            port,
            NULL /* remoteHost */,
            0 /* remotePort */,
            notify,
            sessionID);
}

status_t ANetworkSession::createUDPSession(
        unsigned localPort, const sp<AMessage> &notify, int32_t *sessionID) {
    return createUDPSession(localPort, NULL, 0, notify, sessionID);
}

status_t ANetworkSession::createUDPSession(
        unsigned localPort,
        const char *remoteHost,
        unsigned remotePort,
        const sp<AMessage> &notify,
        int32_t *sessionID) {
    return createClientOrServer(
            kModeCreateUDPSession,
            NULL /* addr */,
            localPort,
            remoteHost,
            remotePort,
            notify,
            sessionID);
}

status_t ANetworkSession::createTCPDatagramSession(
        const struct in_addr &addr, unsigned port,
        const sp<AMessage> &notify, int32_t *sessionID) {
    return createClientOrServer(
            kModeCreateTCPDatagramSessionPassive,
            &addr,
            port,
            NULL /* remoteHost */,
            0 /* remotePort */,
            notify,
            sessionID);
}

status_t ANetworkSession::createTCPDatagramSession(
        unsigned localPort,
        const char *remoteHost,
        unsigned remotePort,
        const sp<AMessage> &notify,
        int32_t *sessionID) {
    return createClientOrServer(
            kModeCreateTCPDatagramSessionActive,
            NULL /* addr */,
            localPort,
            remoteHost,
            remotePort,
            notify,
            sessionID);
}

status_t ANetworkSession::destroySession(int32_t sessionID) {
    Mutex::Autolock autoLock(mLock);

    ssize_t index = mSessions.indexOfKey(sessionID);

    if (index < 0) {
        return -ENOENT;
    }

    mSessions.removeItemsAt(index);

    interrupt();

    return OK;
}

// static
status_t ANetworkSession::MakeSocketNonBlocking(int s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        flags = 0;
    }

    int res = fcntl(s, F_SETFL, flags | O_NONBLOCK);
    if (res < 0) {
        return -errno;
    }

    return OK;
}

status_t ANetworkSession::createClientOrServer(
        Mode mode,
        const struct in_addr *localAddr,
        unsigned port,
        const char *remoteHost,
        unsigned remotePort,
        const sp<AMessage> &notify,
        int32_t *sessionID) {
    Mutex::Autolock autoLock(mLock);

    *sessionID = 0;
    status_t err = OK;
    int s, res;
    sp<Session> session;

    s = socket(
            AF_INET,
            (mode == kModeCreateUDPSession) ? SOCK_DGRAM : SOCK_STREAM,
            0);

    if (s < 0) {
        err = -errno;
        goto bail;
    }

    if (mode == kModeCreateRTSPServer
            || mode == kModeCreateTCPDatagramSessionPassive) {
        const int yes = 1;
        res = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (res < 0) {
            err = -errno;
            goto bail2;
        }
    }

    if (mode == kModeCreateUDPSession) {
        int size = 256 * 1024;

        res = setsockopt(s, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

        if (res < 0) {
            err = -errno;
            goto bail2;
        }

        res = setsockopt(s, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));

        if (res < 0) {
            err = -errno;
            goto bail2;
        }
    }

    err = MakeSocketNonBlocking(s);

    if (err != OK) {
        goto bail2;
    }

    struct sockaddr_in addr;
    memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
    addr.sin_family = AF_INET;

    if (mode == kModeCreateRTSPClient
            || mode == kModeCreateTCPDatagramSessionActive) {
        struct hostent *ent= gethostbyname(remoteHost);
        if (ent == NULL) {
            err = -h_errno;
            goto bail2;
        }

        addr.sin_addr.s_addr = *(in_addr_t *)ent->h_addr;
        addr.sin_port = htons(remotePort);
    } else if (localAddr != NULL) {
        addr.sin_addr = *localAddr;
        addr.sin_port = htons(port);
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
    }

    if (mode == kModeCreateRTSPClient
            || mode == kModeCreateTCPDatagramSessionActive) {
        in_addr_t x = ntohl(addr.sin_addr.s_addr);
        ALOGI("connecting socket %d to %d.%d.%d.%d:%d",
              s,
              (x >> 24),
              (x >> 16) & 0xff,
              (x >> 8) & 0xff,
              x & 0xff,
              ntohs(addr.sin_port));

        res = connect(s, (const struct sockaddr *)&addr, sizeof(addr));

        CHECK_LT(res, 0);
        if (errno == EINPROGRESS) {
            res = 0;
        }
    } else {
        res = bind(s, (const struct sockaddr *)&addr, sizeof(addr));

        if (res == 0) {
            if (mode == kModeCreateRTSPServer
                    || mode == kModeCreateTCPDatagramSessionPassive) {
                res = listen(s, 4);
            } else {
                CHECK_EQ(mode, kModeCreateUDPSession);

                if (remoteHost != NULL) {
                    struct sockaddr_in remoteAddr;
                    memset(remoteAddr.sin_zero, 0, sizeof(remoteAddr.sin_zero));
                    remoteAddr.sin_family = AF_INET;
                    remoteAddr.sin_port = htons(remotePort);

                    struct hostent *ent= gethostbyname(remoteHost);
                    if (ent == NULL) {
                        err = -h_errno;
                        goto bail2;
                    }

                    remoteAddr.sin_addr.s_addr = *(in_addr_t *)ent->h_addr;

                    res = connect(
                            s,
                            (const struct sockaddr *)&remoteAddr,
                            sizeof(remoteAddr));
                }
            }
        }
    }

    if (res < 0) {
        err = -errno;
        goto bail2;
    }

    Session::State state;
    switch (mode) {
        case kModeCreateRTSPClient:
            state = Session::CONNECTING;
            break;

        case kModeCreateTCPDatagramSessionActive:
            state = Session::CONNECTING;
            break;

        case kModeCreateTCPDatagramSessionPassive:
            state = Session::LISTENING_TCP_DGRAMS;
            break;

        case kModeCreateRTSPServer:
            state = Session::LISTENING_RTSP;
            break;

        default:
            CHECK_EQ(mode, kModeCreateUDPSession);
            state = Session::DATAGRAM;
            break;
    }

    session = new Session(
            mNextSessionID++,
            state,
            s,
            notify);

    if (mode == kModeCreateTCPDatagramSessionActive) {
        session->setIsRTSPConnection(false);
    } else if (mode == kModeCreateRTSPClient) {
        session->setIsRTSPConnection(true);
    }

    mSessions.add(session->sessionID(), session);

    interrupt();

    *sessionID = session->sessionID();

    goto bail;

bail2:
    close(s);
    s = -1;

bail:
    return err;
}

status_t ANetworkSession::connectUDPSession(
        int32_t sessionID, const char *remoteHost, unsigned remotePort) {
    Mutex::Autolock autoLock(mLock);

    ssize_t index = mSessions.indexOfKey(sessionID);

    if (index < 0) {
        return -ENOENT;
    }

    const sp<Session> session = mSessions.valueAt(index);
    int s = session->socket();

    struct sockaddr_in remoteAddr;
    memset(remoteAddr.sin_zero, 0, sizeof(remoteAddr.sin_zero));
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(remotePort);

    status_t err = OK;
    struct hostent *ent = gethostbyname(remoteHost);
    if (ent == NULL) {
        err = -h_errno;
    } else {
        remoteAddr.sin_addr.s_addr = *(in_addr_t *)ent->h_addr;

        int res = connect(
                s,
                (const struct sockaddr *)&remoteAddr,
                sizeof(remoteAddr));

        if (res < 0) {
            err = -errno;
        }
    }

    return err;
}

status_t ANetworkSession::sendRequest(
        int32_t sessionID, const void *data, ssize_t size) {
    Mutex::Autolock autoLock(mLock);

    ssize_t index = mSessions.indexOfKey(sessionID);

    if (index < 0) {
        return -ENOENT;
    }

    const sp<Session> session = mSessions.valueAt(index);

    status_t err = session->sendRequest(data, size);

    interrupt();

    return err;
}

void ANetworkSession::interrupt() {
    static const char dummy = 0;

    ssize_t n;
    do {
        n = write(mPipeFd[1], &dummy, 1);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        ALOGW("Error writing to pipe (%s)", strerror(errno));
    }
}

void ANetworkSession::threadLoop() {
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);

    FD_SET(mPipeFd[0], &rs);
    int maxFd = mPipeFd[0];

    {
        Mutex::Autolock autoLock(mLock);

        for (size_t i = 0; i < mSessions.size(); ++i) {
            const sp<Session> &session = mSessions.valueAt(i);

            int s = session->socket();

            if (s < 0) {
                continue;
            }

            if (session->wantsToRead()) {
                FD_SET(s, &rs);
                if (s > maxFd) {
                    maxFd = s;
                }
            }

            if (session->wantsToWrite()) {
                FD_SET(s, &ws);
                if (s > maxFd) {
                    maxFd = s;
                }
            }
        }
    }

    int res = select(maxFd + 1, &rs, &ws, NULL, NULL /* tv */);

    if (res == 0) {
        return;
    }

    if (res < 0) {
        if (errno == EINTR) {
            return;
        }

        ALOGE("select failed w/ error %d (%s)", errno, strerror(errno));
        return;
    }

    if (FD_ISSET(mPipeFd[0], &rs)) {
        char c;
        ssize_t n;
        do {
            n = read(mPipeFd[0], &c, 1);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            ALOGW("Error reading from pipe (%s)", strerror(errno));
        }

        --res;
    }

    {
        Mutex::Autolock autoLock(mLock);

        List<sp<Session> > sessionsToAdd;

        for (size_t i = mSessions.size(); res > 0 && i-- > 0;) {
            const sp<Session> &session = mSessions.valueAt(i);

            int s = session->socket();

            if (s < 0) {
                continue;
            }

            if (FD_ISSET(s, &rs) || FD_ISSET(s, &ws)) {
                --res;
            }

            if (FD_ISSET(s, &rs)) {
                if (session->isRTSPServer() || session->isTCPDatagramServer()) {
                    struct sockaddr_in remoteAddr;
                    socklen_t remoteAddrLen = sizeof(remoteAddr);

                    int clientSocket = accept(
                            s, (struct sockaddr *)&remoteAddr, &remoteAddrLen);

                    if (clientSocket >= 0) {
                        status_t err = MakeSocketNonBlocking(clientSocket);

                        if (err != OK) {
                            ALOGE("Unable to make client socket non blocking, "
                                  "failed w/ error %d (%s)",
                                  err, strerror(-err));

                            close(clientSocket);
                            clientSocket = -1;
                        } else {
                            in_addr_t addr = ntohl(remoteAddr.sin_addr.s_addr);

                            ALOGI("incoming connection from %d.%d.%d.%d:%d "
                                  "(socket %d)",
                                  (addr >> 24),
                                  (addr >> 16) & 0xff,
                                  (addr >> 8) & 0xff,
                                  addr & 0xff,
                                  ntohs(remoteAddr.sin_port),
                                  clientSocket);

                            sp<Session> clientSession =
                                // using socket sd as sessionID
                                new Session(
                                        mNextSessionID++,
                                        Session::CONNECTED,
                                        clientSocket,
                                        session->getNotificationMessage());

                            clientSession->setIsRTSPConnection(
                                    session->isRTSPServer());

                            sessionsToAdd.push_back(clientSession);
                        }
                    } else {
                        ALOGE("accept returned error %d (%s)",
                              errno, strerror(errno));
                    }
                } else {
                    status_t err = session->readMore();
                    if (err != OK) {
                        ALOGE("readMore on socket %d failed w/ error %d (%s)",
                              s, err, strerror(-err));
                    }
                }
            }

            if (FD_ISSET(s, &ws)) {
                status_t err = session->writeMore();
                if (err != OK) {
                    ALOGE("writeMore on socket %d failed w/ error %d (%s)",
                          s, err, strerror(-err));
                }
            }
        }

        while (!sessionsToAdd.empty()) {
            sp<Session> session = *sessionsToAdd.begin();
            sessionsToAdd.erase(sessionsToAdd.begin());

            mSessions.add(session->sessionID(), session);

            ALOGI("added clientSession %d", session->sessionID());
        }
    }
}

}  // namespace android

