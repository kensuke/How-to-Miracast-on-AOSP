#include <jni.h>
#include <string.h>
#include <cstring>

//#define LOG_NDEBUG 0
#define LOG_TAG "Mira_for_You_JNI"
#include <utils/Log.h>

#include "sink/WifiDisplaySink.h"
#include "source/WifiDisplaySource.h"

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <media/AudioSystem.h>
#include <media/IMediaPlayerService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <ui/DisplayInfo.h>

// base src:/frameworks/av/media/libstagefright/wifi-display/wfd.cpp

namespace android {

// Sink JNI

// Called from p2p sequence
extern "C" void Java_com_example_mira4u_P2pSinkActivity_nativeInvokeSink(JNIEnv* env, jobject thiz, jstring ipaddr, jint port, jint special, jint is_N10) {

    ProcessState::self()->startThreadPool();
    DataSource::RegisterDefaultSniffers();

    bool specialMode = special == 1;
    bool isN10 = is_N10 == 1;

    sp<SurfaceComposerClient> composerClient = new SurfaceComposerClient;
    CHECK_EQ(composerClient->initCheck(), (status_t)OK);

    sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain));
    DisplayInfo info;
    SurfaceComposerClient::getDisplayInfo(display, &info);
    ssize_t displayWidth = info.w;
    ssize_t displayHeight = info.h;
    ALOGD("Sink Display[%d, %d] Special[%d] Nexus10[%d]", displayWidth, displayHeight, specialMode, isN10);

    sp<SurfaceControl> control =
        composerClient->createSurface(
                String8("A Sink Surface"),
//                displayWidth,
//                displayHeight,
                // in Sink is Nexus 10, reverse width & height params good working. Why?
                isN10 ? displayHeight : displayWidth,
                isN10 ? displayWidth : displayHeight,
                PIXEL_FORMAT_RGB_565,
                0);

    CHECK(control != NULL);
    CHECK(control->isValid());

    SurfaceComposerClient::openGlobalTransaction();
    CHECK_EQ(control->setLayer(INT_MAX), (status_t)OK);
    CHECK_EQ(control->show(), (status_t)OK);
    SurfaceComposerClient::closeGlobalTransaction();

    sp<Surface> surface = control->getSurface();
    CHECK(surface != NULL);

    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;

    sp<WifiDisplaySink> sink = new WifiDisplaySink(
            specialMode ? WifiDisplaySink::FLAG_SPECIAL_MODE : 0 /* flags */,
            session,
            surface->getIGraphicBufferProducer());

    looper->registerHandler(sink);

    const char *ip = env->GetStringUTFChars(ipaddr, NULL);
    ALOGD("Source Addr[%s] Port[%d]", ip, port);

    // @TODO
    //if (connectToPort >= 0) {
    //    sink->start(connectToHost.c_str(), connectToPort);
    //} else {
    //    sink->start(uri.c_str());
    //}
    sink->start(ip, port);

    looper->start(true /* runOnCallingThread */);

    composerClient->dispose();
}

// old func
extern "C" void Java_com_example_mira4u_WfdActivity_nativeInvokeSink(JNIEnv* env, jobject thiz, jstring ipaddr, jint port) {
    Java_com_example_mira4u_P2pSinkActivity_nativeInvokeSink(env, thiz, ipaddr, port, 0, 0);
}


// Source lib
struct RemoteDisplayClient : public BnRemoteDisplayClient {
    RemoteDisplayClient();

    virtual void onDisplayConnected(
            const sp<IGraphicBufferProducer> &bufferProducer,
            uint32_t width,
            uint32_t height,
            uint32_t flags,
            uint32_t session);

    virtual void onDisplayDisconnected();
    virtual void onDisplayError(int32_t error);

    void waitUntilDone();

protected:
    virtual ~RemoteDisplayClient();

private:
    Mutex mLock;
    Condition mCondition;

    bool mDone;

    sp<SurfaceComposerClient> mComposerClient;
    sp<IGraphicBufferProducer> mSurfaceTexture;
    sp<IBinder> mDisplayBinder;

    DISALLOW_EVIL_CONSTRUCTORS(RemoteDisplayClient);
};

RemoteDisplayClient::RemoteDisplayClient()
    : mDone(false) {
    mComposerClient = new SurfaceComposerClient;
    CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);
}

RemoteDisplayClient::~RemoteDisplayClient() {
}

void RemoteDisplayClient::onDisplayConnected(
        const sp<IGraphicBufferProducer> &bufferProducer,
        uint32_t width,
        uint32_t height,
        uint32_t flags,
        uint32_t session) {
    ALOGI("onDisplayConnected width=%u, height=%u, flags = 0x%08x, session = %d",
          width, height, flags, session);

    if (bufferProducer != NULL) {
        mSurfaceTexture = bufferProducer;
        mDisplayBinder = mComposerClient->createDisplay(
                String8("foo"), false /* secure */);

        SurfaceComposerClient::openGlobalTransaction();
        mComposerClient->setDisplaySurface(mDisplayBinder, mSurfaceTexture);

        Rect layerStackRect(1280, 720);  // XXX fix this.
        Rect displayRect(1280, 720);

        mComposerClient->setDisplayProjection(
                mDisplayBinder, 0 /* 0 degree rotation */,
                layerStackRect,
                displayRect);

        SurfaceComposerClient::closeGlobalTransaction();
    }
}

void RemoteDisplayClient::onDisplayDisconnected() {
    ALOGI("onDisplayDisconnected");

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::onDisplayError(int32_t error) {
    ALOGI("onDisplayError error=%d", error);

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::waitUntilDone() {
    Mutex::Autolock autoLock(mLock);
    while (!mDone) {
        mCondition.wait(mLock);
    }
}

static void createSource(const AString &addr, int32_t port) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    CHECK(service.get() != NULL);

    String8 iface;
    iface.append(addr.c_str());
    iface.append(StringPrintf(":%d", port).c_str());

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<IRemoteDisplay> display =
        service->listenForRemoteDisplay(client, iface);

    client->waitUntilDone();

    display->dispose();
    display.clear();
}

static void createFileSource(
        const AString &addr, int32_t port, const char *path) {
    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;
    looper->start();

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<WifiDisplaySource> source = new WifiDisplaySource(session, client, path);
    looper->registerHandler(source);

    AString iface = StringPrintf("%s:%d", addr.c_str(), port);
    CHECK_EQ((status_t)OK, source->start(iface.c_str()));

    client->waitUntilDone();

    source->stop();
}

// Source JNI
// NO TESTED
extern "C" void Java_com_example_mira4u_WfdActivity_nativeInvokeSource(JNIEnv* env, jobject thiz, jstring ipaddr, jint port) {
    ProcessState::self()->startThreadPool();
    DataSource::RegisterDefaultSniffers();

    const char *ip = env->GetStringUTFChars(ipaddr, NULL);
    ALOGD("Source[%s] Port[%d]", ip, port);

    createSource(ip, port);
}

}  // namespace android
