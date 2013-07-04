LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := $(call all-subdir-java-files)

LOCAL_PACKAGE_NAME := Mira4U
LOCAL_CERTIFICATE := platform

LOCAL_JNI_SHARED_LIBRARIES := libMira4U
LOCAL_REQUIRED_MODULES := libMira4U

include $(BUILD_PACKAGE)
include $(call all-makefiles-under, $(LOCAL_PATH))
