/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "SurfaceTexture"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <hardware/hardware.h>

#include <gui/IGraphicBufferAlloc.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceTexture.h>

#include <private/gui/ComposerService.h>

#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Trace.h>

// This compile option makes SurfaceTexture use the
// EGL_ANDROID_native_fence_sync extension to create Android native fences to
// signal when all GLES reads for a given buffer have completed.  It is not
// compatible with using the EGL_KHR_fence_sync extension for the same
// purpose.
#ifdef USE_NATIVE_FENCE_SYNC
#ifdef USE_FENCE_SYNC
#error "USE_NATIVE_FENCE_SYNC and USE_FENCE_SYNC are incompatible"
#endif
static const bool useNativeFenceSync = true;
#else
static const bool useNativeFenceSync = false;
#endif

// This compile option makes SurfaceTexture use the EGL_ANDROID_sync_wait
// extension to insert server-side waits into the GLES command stream.  This
// feature requires the EGL_ANDROID_native_fence_sync and
// EGL_ANDROID_wait_sync extensions.
#ifdef USE_WAIT_SYNC
static const bool useWaitSync = true;
#else
static const bool useWaitSync = false;
#endif

// Macros for including the SurfaceTexture name in log messages
#define ST_LOGV(x, ...) ALOGV("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGD(x, ...) ALOGD("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGI(x, ...) ALOGI("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGW(x, ...) ALOGW("[%s] "x, mName.string(), ##__VA_ARGS__)
#define ST_LOGE(x, ...) ALOGE("[%s] "x, mName.string(), ##__VA_ARGS__)

namespace android {

// Transform matrices
static float mtxIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};
static float mtxFlipH[16] = {
    -1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxFlipV[16] = {
    1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};
static float mtxRot90[16] = {
    0, 1, 0, 0,
    -1, 0, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
};
static float mtxRot180[16] = {
    -1, 0, 0, 0,
    0, -1, 0, 0,
    0, 0, 1, 0,
    1, 1, 0, 1,
};
static float mtxRot270[16] = {
    0, -1, 0, 0,
    1, 0, 0, 0,
    0, 0, 1, 0,
    0, 1, 0, 1,
};

static void mtxMul(float out[16], const float a[16], const float b[16]);


SurfaceTexture::SurfaceTexture(GLuint tex, bool allowSynchronousMode,
        GLenum texTarget, bool useFenceSync, const sp<BufferQueue> &bufferQueue) :
    ConsumerBase(bufferQueue == 0 ? new BufferQueue(allowSynchronousMode) : bufferQueue),
    mCurrentTransform(0),
    mCurrentTimestamp(0),
    mFilteringEnabled(true),
    mTexName(tex),
#ifdef USE_FENCE_SYNC
    mUseFenceSync(useFenceSync),
#else
    mUseFenceSync(false),
#endif
    mTexTarget(texTarget),
    mEglDisplay(EGL_NO_DISPLAY),
    mEglContext(EGL_NO_CONTEXT),
    mCurrentTexture(BufferQueue::INVALID_BUFFER_SLOT),
    mAttached(true)
{
    ST_LOGV("SurfaceTexture");

    memcpy(mCurrentTransformMatrix, mtxIdentity,
            sizeof(mCurrentTransformMatrix));

    mBufferQueue->setConsumerUsageBits(DEFAULT_USAGE_FLAGS);
}

status_t SurfaceTexture::setDefaultMaxBufferCount(int bufferCount) {
    Mutex::Autolock lock(mMutex);
    return mBufferQueue->setDefaultMaxBufferCount(bufferCount);
}


status_t SurfaceTexture::setDefaultBufferSize(uint32_t w, uint32_t h)
{
    Mutex::Autolock lock(mMutex);
    mDefaultWidth = w;
    mDefaultHeight = h;
    return mBufferQueue->setDefaultBufferSize(w, h);
}

status_t SurfaceTexture::updateTexImage() {
    return SurfaceTexture::updateTexImage(NULL, false);
}

status_t SurfaceTexture::acquireBufferLocked(BufferQueue::BufferItem *item) {
    status_t err = ConsumerBase::acquireBufferLocked(item);
    if (err != NO_ERROR) {
        return err;
    }

    int slot = item->mBuf;
    if (item->mGraphicBuffer != NULL) {
        if (mEglSlots[slot].mEglImage != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(mEglDisplay, mEglSlots[slot].mEglImage);
            mEglSlots[slot].mEglImage = EGL_NO_IMAGE_KHR;
        }
    }

    // Update the GL texture object. We may have to do this even when
    // item.mGraphicBuffer == NULL, if we destroyed the EGLImage when
    // detaching from a context but the buffer has not been re-allocated.
    if (mEglSlots[slot].mEglImage == EGL_NO_IMAGE_KHR) {
        EGLImageKHR image = createImage(mEglDisplay, mSlots[slot].mGraphicBuffer);
        if (image == EGL_NO_IMAGE_KHR) {
            return UNKNOWN_ERROR;
        }
        mEglSlots[slot].mEglImage = image;
    }

    return NO_ERROR;
}

status_t SurfaceTexture::releaseBufferLocked(int buf, EGLDisplay display,
       EGLSyncKHR eglFence) {
    status_t err = ConsumerBase::releaseBufferLocked(buf, mEglDisplay,
           eglFence);

    mEglSlots[buf].mEglFence = EGL_NO_SYNC_KHR;

    return err;
}

status_t SurfaceTexture::updateTexImage(BufferRejecter* rejecter, bool skipSync) {
    ATRACE_CALL();
    ST_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);

    status_t err = NO_ERROR;

    if (mAbandoned) {
        ST_LOGE("updateTexImage: SurfaceTexture is abandoned!");
        return NO_INIT;
    }

    if (!mAttached) {
        ST_LOGE("updateTexImage: SurfaceTexture is not attached to an OpenGL "
                "ES context");
        return INVALID_OPERATION;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    if ((mEglDisplay != dpy && mEglDisplay != EGL_NO_DISPLAY) ||
            dpy == EGL_NO_DISPLAY) {
        ST_LOGE("updateTexImage: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if ((mEglContext != ctx && mEglContext != EGL_NO_CONTEXT) ||
            ctx == EGL_NO_CONTEXT) {
        ST_LOGE("updateTexImage: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    mEglDisplay = dpy;
    mEglContext = ctx;

    BufferQueue::BufferItem item;

    // In asynchronous mode the list is guaranteed to be one buffer
    // deep, while in synchronous mode we use the oldest buffer.
    err = acquireBufferLocked(&item);
    if (err == NO_ERROR) {
        int buf = item.mBuf;

        // we call the rejecter here, in case the caller has a reason to
        // not accept this buffer. this is used by SurfaceFlinger to
        // reject buffers which have the wrong size
        if (rejecter && rejecter->reject(mSlots[buf].mGraphicBuffer, item)) {
            releaseBufferLocked(buf, dpy, EGL_NO_SYNC_KHR);
            glBindTexture(mTexTarget, mTexName);
            return NO_ERROR;
        }

        GLint error;
        while ((error = glGetError()) != GL_NO_ERROR) {
            ST_LOGW("updateTexImage: clearing GL error: %#04x", error);
        }

        EGLImageKHR image = mEglSlots[buf].mEglImage;
        glBindTexture(mTexTarget, mTexName);
        glEGLImageTargetTexture2DOES(mTexTarget, (GLeglImageOES)image);

        while ((error = glGetError()) != GL_NO_ERROR) {
            ST_LOGE("updateTexImage: error binding external texture image %p "
                    "(slot %d): %#04x", image, buf, error);
            err = UNKNOWN_ERROR;
        }

        if (err == NO_ERROR) {
            err = syncForReleaseLocked(dpy);
        }

        if (err != NO_ERROR) {
            // Release the buffer we just acquired.  It's not safe to
            // release the old buffer, so instead we just drop the new frame.
            releaseBufferLocked(buf, dpy, EGL_NO_SYNC_KHR);
            return err;
        }

        ST_LOGV("updateTexImage: (slot=%d buf=%p) -> (slot=%d buf=%p)",
                mCurrentTexture,
                mCurrentTextureBuf != NULL ? mCurrentTextureBuf->handle : 0,
                buf, mSlots[buf].mGraphicBuffer->handle);

        // release old buffer
        if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
            status_t status = releaseBufferLocked(mCurrentTexture, dpy,
                    mEglSlots[mCurrentTexture].mEglFence);
            if (status != NO_ERROR && status != BufferQueue::STALE_BUFFER_SLOT) {
                ST_LOGE("updateTexImage: failed to release buffer: %s (%d)",
                       strerror(-status), status);
                err = status;
            }
        }

        // Update the SurfaceTexture state.
        mCurrentTexture = buf;
        mCurrentTextureBuf = mSlots[buf].mGraphicBuffer;
        mCurrentCrop = item.mCrop;
        mCurrentTransform = item.mTransform;
        mCurrentScalingMode = item.mScalingMode;
        mCurrentTimestamp = item.mTimestamp;
        mCurrentFence = item.mFence;
        if (!skipSync) {
            // SurfaceFlinger needs to lazily perform GLES synchronization
            // only when it's actually going to use GLES for compositing.
            // Eventually SurfaceFlinger should have its own consumer class,
            // but for now we'll just hack it in to SurfaceTexture.
            // SurfaceFlinger is responsible for calling doGLFenceWait before
            // texturing from this SurfaceTexture.
            doGLFenceWaitLocked();
        }
        computeCurrentTransformMatrixLocked();
    } else  {
        if (err < 0) {
            ST_LOGE("updateTexImage: acquire failed: %s (%d)",
                strerror(-err), err);
            return err;
        }
        // We always bind the texture even if we don't update its contents.
        glBindTexture(mTexTarget, mTexName);
        return OK;
    }

    return err;
}

void SurfaceTexture::setReleaseFence(int fenceFd) {
    sp<Fence> fence(new Fence(fenceFd));
    if (fenceFd == -1 || mCurrentTexture == BufferQueue::INVALID_BUFFER_SLOT)
        return;
    status_t err = addReleaseFence(mCurrentTexture, fence);
    if (err != OK) {
        ST_LOGE("setReleaseFence: failed to add the fence: %s (%d)",
                strerror(-err), err);
    }
}

status_t SurfaceTexture::detachFromContext() {
    ATRACE_CALL();
    ST_LOGV("detachFromContext");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        ST_LOGE("detachFromContext: abandoned SurfaceTexture");
        return NO_INIT;
    }

    if (!mAttached) {
        ST_LOGE("detachFromContext: SurfaceTexture is not attached to a "
                "context");
        return INVALID_OPERATION;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    if (mEglDisplay != dpy && mEglDisplay != EGL_NO_DISPLAY) {
        ST_LOGE("detachFromContext: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if (mEglContext != ctx && mEglContext != EGL_NO_CONTEXT) {
        ST_LOGE("detachFromContext: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    if (dpy != EGL_NO_DISPLAY && ctx != EGL_NO_CONTEXT) {
        status_t err = syncForReleaseLocked(dpy);
        if (err != OK) {
            return err;
        }

        glDeleteTextures(1, &mTexName);
    }

    // Because we're giving up the EGLDisplay we need to free all the EGLImages
    // that are associated with it.  They'll be recreated when the
    // SurfaceTexture gets attached to a new OpenGL ES context (and thus gets a
    // new EGLDisplay).
    for (int i =0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
        EGLImageKHR img = mEglSlots[i].mEglImage;
        if (img != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(mEglDisplay, img);
            mEglSlots[i].mEglImage = EGL_NO_IMAGE_KHR;
        }
    }

    mEglDisplay = EGL_NO_DISPLAY;
    mEglContext = EGL_NO_CONTEXT;
    mAttached = false;

    return OK;
}

status_t SurfaceTexture::attachToContext(GLuint tex) {
    ATRACE_CALL();
    ST_LOGV("attachToContext");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        ST_LOGE("attachToContext: abandoned SurfaceTexture");
        return NO_INIT;
    }

    if (mAttached) {
        ST_LOGE("attachToContext: SurfaceTexture is already attached to a "
                "context");
        return INVALID_OPERATION;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    if (dpy == EGL_NO_DISPLAY) {
        ST_LOGE("attachToContext: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if (ctx == EGL_NO_CONTEXT) {
        ST_LOGE("attachToContext: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    // We need to bind the texture regardless of whether there's a current
    // buffer.
    glBindTexture(mTexTarget, tex);

    if (mCurrentTextureBuf != NULL) {
        // The EGLImageKHR that was associated with the slot was destroyed when
        // the SurfaceTexture was detached from the old context, so we need to
        // recreate it here.
        EGLImageKHR image = createImage(dpy, mCurrentTextureBuf);
        if (image == EGL_NO_IMAGE_KHR) {
            return UNKNOWN_ERROR;
        }

        // Attach the current buffer to the GL texture.
        glEGLImageTargetTexture2DOES(mTexTarget, (GLeglImageOES)image);

        GLint error;
        status_t err = OK;
        while ((error = glGetError()) != GL_NO_ERROR) {
            ST_LOGE("attachToContext: error binding external texture image %p "
                    "(slot %d): %#04x", image, mCurrentTexture, error);
            err = UNKNOWN_ERROR;
        }

        // We destroy the EGLImageKHR here because the current buffer may no
        // longer be associated with one of the buffer slots, so we have
        // nowhere to to store it.  If the buffer is still associated with a
        // slot then another EGLImageKHR will be created next time that buffer
        // gets acquired in updateTexImage.
        eglDestroyImageKHR(dpy, image);

        if (err != OK) {
            return err;
        }
    }

    mEglDisplay = dpy;
    mEglContext = ctx;
    mTexName = tex;
    mAttached = true;

    return OK;
}

status_t SurfaceTexture::syncForReleaseLocked(EGLDisplay dpy) {
    ST_LOGV("syncForReleaseLocked");

    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (useNativeFenceSync) {
            EGLSyncKHR sync = eglCreateSyncKHR(dpy,
                    EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            if (sync == EGL_NO_SYNC_KHR) {
                ST_LOGE("syncForReleaseLocked: error creating EGL fence: %#x",
                        eglGetError());
                return UNKNOWN_ERROR;
            }
            glFlush();
            int fenceFd = eglDupNativeFenceFDANDROID(dpy, sync);
            eglDestroySyncKHR(dpy, sync);
            if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
                ST_LOGE("syncForReleaseLocked: error dup'ing native fence "
                        "fd: %#x", eglGetError());
                return UNKNOWN_ERROR;
            }
            sp<Fence> fence(new Fence(fenceFd));
            status_t err = addReleaseFenceLocked(mCurrentTexture, fence);
            if (err != OK) {
                ST_LOGE("syncForReleaseLocked: error adding release fence: "
                        "%s (%d)", strerror(-err), err);
                return err;
            }
        } else if (mUseFenceSync) {
            EGLSyncKHR fence = mEglSlots[mCurrentTexture].mEglFence;
            if (fence != EGL_NO_SYNC_KHR) {
                // There is already a fence for the current slot.  We need to
                // wait on that before replacing it with another fence to
                // ensure that all outstanding buffer accesses have completed
                // before the producer accesses it.
                EGLint result = eglClientWaitSyncKHR(dpy, fence, 0, 1000000000);
                if (result == EGL_FALSE) {
                    ST_LOGE("syncForReleaseLocked: error waiting for previous "
                            "fence: %#x", eglGetError());
                    return UNKNOWN_ERROR;
                } else if (result == EGL_TIMEOUT_EXPIRED_KHR) {
                    ST_LOGE("syncForReleaseLocked: timeout waiting for previous "
                            "fence");
                    return TIMED_OUT;
                }
                eglDestroySyncKHR(dpy, fence);
            }

            // Create a fence for the outstanding accesses in the current
            // OpenGL ES context.
            fence = eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
            if (fence == EGL_NO_SYNC_KHR) {
                ST_LOGE("syncForReleaseLocked: error creating fence: %#x",
                        eglGetError());
                return UNKNOWN_ERROR;
            }
            glFlush();
            mEglSlots[mCurrentTexture].mEglFence = fence;
        }
    }

    return OK;
}

bool SurfaceTexture::isExternalFormat(uint32_t format)
{
    switch (format) {
    // supported YUV formats
    case HAL_PIXEL_FORMAT_YV12:
    // Legacy/deprecated YUV formats
    case HAL_PIXEL_FORMAT_YCbCr_422_SP:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_422_I:
        return true;
    }

    // Any OEM format needs to be considered
    if (format>=0x100 && format<=0x1FF)
        return true;

    return false;
}

GLenum SurfaceTexture::getCurrentTextureTarget() const {
    return mTexTarget;
}

void SurfaceTexture::getTransformMatrix(float mtx[16]) {
    Mutex::Autolock lock(mMutex);
    memcpy(mtx, mCurrentTransformMatrix, sizeof(mCurrentTransformMatrix));
}

void SurfaceTexture::setFilteringEnabled(bool enabled) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        ST_LOGE("setFilteringEnabled: SurfaceTexture is abandoned!");
        return;
    }
    bool needsRecompute = mFilteringEnabled != enabled;
    mFilteringEnabled = enabled;

    if (needsRecompute && mCurrentTextureBuf==NULL) {
        ST_LOGD("setFilteringEnabled called with mCurrentTextureBuf == NULL");
    }

    if (needsRecompute && mCurrentTextureBuf != NULL) {
        computeCurrentTransformMatrixLocked();
    }
}

void SurfaceTexture::computeCurrentTransformMatrixLocked() {
    ST_LOGV("computeCurrentTransformMatrixLocked");

    float xform[16];
    for (int i = 0; i < 16; i++) {
        xform[i] = mtxIdentity[i];
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        float result[16];
        mtxMul(result, xform, mtxFlipH);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        float result[16];
        mtxMul(result, xform, mtxFlipV);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        float result[16];
        mtxMul(result, xform, mtxRot90);
        for (int i = 0; i < 16; i++) {
            xform[i] = result[i];
        }
    }

    sp<GraphicBuffer>& buf(mCurrentTextureBuf);

    if (buf == NULL) {
        ST_LOGD("computeCurrentTransformMatrixLocked: mCurrentTextureBuf is NULL");
    }

    Rect cropRect = mCurrentCrop;
    float tx = 0.0f, ty = 0.0f, sx = 1.0f, sy = 1.0f;
    float bufferWidth = buf->getWidth();
    float bufferHeight = buf->getHeight();
    if (!cropRect.isEmpty()) {
        float shrinkAmount = 0.0f;
        if (mFilteringEnabled) {
            // In order to prevent bilinear sampling beyond the edge of the
            // crop rectangle we may need to shrink it by 2 texels in each
            // dimension.  Normally this would just need to take 1/2 a texel
            // off each end, but because the chroma channels of YUV420 images
            // are subsampled we may need to shrink the crop region by a whole
            // texel on each side.
            switch (buf->getPixelFormat()) {
                case PIXEL_FORMAT_RGBA_8888:
                case PIXEL_FORMAT_RGBX_8888:
                case PIXEL_FORMAT_RGB_888:
                case PIXEL_FORMAT_RGB_565:
                case PIXEL_FORMAT_BGRA_8888:
                case PIXEL_FORMAT_RGBA_5551:
                case PIXEL_FORMAT_RGBA_4444:
                    // We know there's no subsampling of any channels, so we
                    // only need to shrink by a half a pixel.
                    shrinkAmount = 0.5;
                    break;

                default:
                    // If we don't recognize the format, we must assume the
                    // worst case (that we care about), which is YUV420.
                    shrinkAmount = 1.0;
                    break;
            }
        }

        // Only shrink the dimensions that are not the size of the buffer.
        if (cropRect.width() < bufferWidth) {
            tx = (float(cropRect.left) + shrinkAmount) / bufferWidth;
            sx = (float(cropRect.width()) - (2.0f * shrinkAmount)) /
                    bufferWidth;
        }
        if (cropRect.height() < bufferHeight) {
            ty = (float(bufferHeight - cropRect.bottom) + shrinkAmount) /
                    bufferHeight;
            sy = (float(cropRect.height()) - (2.0f * shrinkAmount)) /
                    bufferHeight;
        }
    }
    float crop[16] = {
        sx, 0, 0, 0,
        0, sy, 0, 0,
        0, 0, 1, 0,
        tx, ty, 0, 1,
    };

    float mtxBeforeFlipV[16];
    mtxMul(mtxBeforeFlipV, crop, xform);

    // SurfaceFlinger expects the top of its window textures to be at a Y
    // coordinate of 0, so SurfaceTexture must behave the same way.  We don't
    // want to expose this to applications, however, so we must add an
    // additional vertical flip to the transform after all the other transforms.
    mtxMul(mCurrentTransformMatrix, mtxFlipV, mtxBeforeFlipV);
}

nsecs_t SurfaceTexture::getTimestamp() {
    ST_LOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

EGLImageKHR SurfaceTexture::createImage(EGLDisplay dpy,
        const sp<GraphicBuffer>& graphicBuffer) {
    EGLClientBuffer cbuf = (EGLClientBuffer)graphicBuffer->getNativeBuffer();
    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR,    EGL_TRUE,
        EGL_NONE,
    };
    EGLImageKHR image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        ST_LOGE("error creating EGLImage: %#x", error);
    }
    return image;
}

sp<GraphicBuffer> SurfaceTexture::getCurrentBuffer() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTextureBuf;
}

Rect SurfaceTexture::getCurrentCrop() const {
    Mutex::Autolock lock(mMutex);

    Rect outCrop = mCurrentCrop;
    if (mCurrentScalingMode == NATIVE_WINDOW_SCALING_MODE_SCALE_CROP) {
        int32_t newWidth = mCurrentCrop.width();
        int32_t newHeight = mCurrentCrop.height();

        if (newWidth * mDefaultHeight > newHeight * mDefaultWidth) {
            newWidth = newHeight * mDefaultWidth / mDefaultHeight;
            ST_LOGV("too wide: newWidth = %d", newWidth);
        } else if (newWidth * mDefaultHeight < newHeight * mDefaultWidth) {
            newHeight = newWidth * mDefaultHeight / mDefaultWidth;
            ST_LOGV("too tall: newHeight = %d", newHeight);
        }

        // The crop is too wide
        if (newWidth < mCurrentCrop.width()) {
            int32_t dw = (newWidth - mCurrentCrop.width())/2;
            outCrop.left -=dw;
            outCrop.right += dw;
        // The crop is too tall
        } else if (newHeight < mCurrentCrop.height()) {
            int32_t dh = (newHeight - mCurrentCrop.height())/2;
            outCrop.top -= dh;
            outCrop.bottom += dh;
        }

        ST_LOGV("getCurrentCrop final crop [%d,%d,%d,%d]",
            outCrop.left, outCrop.top,
            outCrop.right,outCrop.bottom);
    }

    return outCrop;
}

uint32_t SurfaceTexture::getCurrentTransform() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransform;
}

uint32_t SurfaceTexture::getCurrentScalingMode() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentScalingMode;
}

sp<Fence> SurfaceTexture::getCurrentFence() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFence;
}

status_t SurfaceTexture::doGLFenceWait() const {
    Mutex::Autolock lock(mMutex);
    return doGLFenceWaitLocked();
}

status_t SurfaceTexture::doGLFenceWaitLocked() const {

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();

    if (mEglDisplay != dpy || mEglDisplay == EGL_NO_DISPLAY) {
        ST_LOGE("doGLFenceWait: invalid current EGLDisplay");
        return INVALID_OPERATION;
    }

    if (mEglContext != ctx || mEglContext == EGL_NO_CONTEXT) {
        ST_LOGE("doGLFenceWait: invalid current EGLContext");
        return INVALID_OPERATION;
    }

    if (mCurrentFence != NULL) {
        if (useWaitSync) {
            // Create an EGLSyncKHR from the current fence.
            int fenceFd = mCurrentFence->dup();
            if (fenceFd == -1) {
                ST_LOGE("doGLFenceWait: error dup'ing fence fd: %d", errno);
                return -errno;
            }
            EGLint attribs[] = {
                EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd,
                EGL_NONE
            };
            EGLSyncKHR sync = eglCreateSyncKHR(dpy,
                    EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
            if (sync == EGL_NO_SYNC_KHR) {
                close(fenceFd);
                ST_LOGE("doGLFenceWait: error creating EGL fence: %#x",
                        eglGetError());
                return UNKNOWN_ERROR;
            }

            // XXX: The spec draft is inconsistent as to whether this should
            // return an EGLint or void.  Ignore the return value for now, as
            // it's not strictly needed.
            eglWaitSyncANDROID(dpy, sync, 0);
            EGLint eglErr = eglGetError();
            eglDestroySyncKHR(dpy, sync);
            if (eglErr != EGL_SUCCESS) {
                ST_LOGE("doGLFenceWait: error waiting for EGL fence: %#x",
                        eglErr);
                return UNKNOWN_ERROR;
            }
        } else {
            status_t err = mCurrentFence->waitForever(1000,
                    "SurfaceTexture::doGLFenceWaitLocked");
            if (err != NO_ERROR) {
                ST_LOGE("doGLFenceWait: error waiting for fence: %d", err);
                return err;
            }
        }
    }

    return NO_ERROR;
}

bool SurfaceTexture::isSynchronousMode() const {
    Mutex::Autolock lock(mMutex);
    return mBufferQueue->isSynchronousMode();
}

void SurfaceTexture::freeBufferLocked(int slotIndex) {
    ST_LOGV("freeBufferLocked: slotIndex=%d", slotIndex);
    if (slotIndex == mCurrentTexture) {
        mCurrentTexture = BufferQueue::INVALID_BUFFER_SLOT;
    }
    EGLImageKHR img = mEglSlots[slotIndex].mEglImage;
    if (img != EGL_NO_IMAGE_KHR) {
        ST_LOGV("destroying EGLImage dpy=%p img=%p", mEglDisplay, img);
        eglDestroyImageKHR(mEglDisplay, img);
    }
    mEglSlots[slotIndex].mEglImage = EGL_NO_IMAGE_KHR;
    ConsumerBase::freeBufferLocked(slotIndex);
}

void SurfaceTexture::abandonLocked() {
    ST_LOGV("abandonLocked");
    mCurrentTextureBuf.clear();
    ConsumerBase::abandonLocked();
}

void SurfaceTexture::setName(const String8& name) {
    Mutex::Autolock _l(mMutex);
    mName = name;
    mBufferQueue->setConsumerName(name);
}

status_t SurfaceTexture::setDefaultBufferFormat(uint32_t defaultFormat) {
    Mutex::Autolock lock(mMutex);
    return mBufferQueue->setDefaultBufferFormat(defaultFormat);
}

status_t SurfaceTexture::setConsumerUsageBits(uint32_t usage) {
    Mutex::Autolock lock(mMutex);
    usage |= DEFAULT_USAGE_FLAGS;
    return mBufferQueue->setConsumerUsageBits(usage);
}

status_t SurfaceTexture::setTransformHint(uint32_t hint) {
    Mutex::Autolock lock(mMutex);
    return mBufferQueue->setTransformHint(hint);
}

// Used for refactoring BufferQueue from SurfaceTexture
// Should not be in final interface once users of SurfaceTexture are clean up.
status_t SurfaceTexture::setSynchronousMode(bool enabled) {
    Mutex::Autolock lock(mMutex);
    return mBufferQueue->setSynchronousMode(enabled);
}

void SurfaceTexture::dumpLocked(String8& result, const char* prefix,
        char* buffer, size_t size) const
{
    snprintf(buffer, size,
       "%smTexName=%d mCurrentTexture=%d\n"
       "%smCurrentCrop=[%d,%d,%d,%d] mCurrentTransform=%#x\n",
       prefix, mTexName, mCurrentTexture, prefix, mCurrentCrop.left,
       mCurrentCrop.top, mCurrentCrop.right, mCurrentCrop.bottom,
       mCurrentTransform);
    result.append(buffer);

    ConsumerBase::dumpLocked(result, prefix, buffer, size);
}

static void mtxMul(float out[16], const float a[16], const float b[16]) {
    out[0] = a[0]*b[0] + a[4]*b[1] + a[8]*b[2] + a[12]*b[3];
    out[1] = a[1]*b[0] + a[5]*b[1] + a[9]*b[2] + a[13]*b[3];
    out[2] = a[2]*b[0] + a[6]*b[1] + a[10]*b[2] + a[14]*b[3];
    out[3] = a[3]*b[0] + a[7]*b[1] + a[11]*b[2] + a[15]*b[3];

    out[4] = a[0]*b[4] + a[4]*b[5] + a[8]*b[6] + a[12]*b[7];
    out[5] = a[1]*b[4] + a[5]*b[5] + a[9]*b[6] + a[13]*b[7];
    out[6] = a[2]*b[4] + a[6]*b[5] + a[10]*b[6] + a[14]*b[7];
    out[7] = a[3]*b[4] + a[7]*b[5] + a[11]*b[6] + a[15]*b[7];

    out[8] = a[0]*b[8] + a[4]*b[9] + a[8]*b[10] + a[12]*b[11];
    out[9] = a[1]*b[8] + a[5]*b[9] + a[9]*b[10] + a[13]*b[11];
    out[10] = a[2]*b[8] + a[6]*b[9] + a[10]*b[10] + a[14]*b[11];
    out[11] = a[3]*b[8] + a[7]*b[9] + a[11]*b[10] + a[15]*b[11];

    out[12] = a[0]*b[12] + a[4]*b[13] + a[8]*b[14] + a[12]*b[15];
    out[13] = a[1]*b[12] + a[5]*b[13] + a[9]*b[14] + a[13]*b[15];
    out[14] = a[2]*b[12] + a[6]*b[13] + a[10]*b[14] + a[14]*b[15];
    out[15] = a[3]*b[12] + a[7]*b[13] + a[11]*b[14] + a[15]*b[15];
}

}; // namespace android
