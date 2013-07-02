LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := libMira4U
LOCAL_SRC_FILES := Mira4U.cpp

LOCAL_C_INCLUDES += \
    $(JNI_H_INCLUDE) \
    $(TOP)/frameworks/native/include \
    $(TOP)/frameworks/native/include/media/openmax \
    $(TOP)/frameworks/base/include \
    $(TOP)/frameworks/av/include/media/stagefright/foundation \
    $(TOP)/frameworks/av/media/libstagefright/wifi-display

LOCAL_SHARED_LIBRARIES:= \
    libbinder                       \
    libgui                          \
    libmedia                        \
    libstagefright                  \
    libstagefright_foundation       \
    libstagefright_wfd              \
    libutils                        \

LOCAL_CERTIFICATE := platform

include $(BUILD_SHARED_LIBRARY)
