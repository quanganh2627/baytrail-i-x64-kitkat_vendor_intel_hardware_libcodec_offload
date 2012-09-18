# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

# The default audio HAL module, which is a stub, that is loaded if no other
# device specific modules are present. The exact load order can be seen in
# libhardware/hardware.c
#
# The format of the name is audio.<type>.<hardware/etc>.so where the only
# required type is 'primary'. Other possibilites are 'a2dp', 'usb', etc.
include $(CLEAR_VARS)

LOCAL_MODULE := audio.codec_offload.$(TARGET_DEVICE)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_CFLAGS := -std=c99
LOCAL_SRC_FILES := codec_offload_hal.c
LOCAL_SHARED_LIBRARIES := liblog libcutils \
                          libutils \
                          libasound \
                          libtinycompress

LOCAL_STATIC_LIBRARIES := libmedia_helper
LOCAL_MODULE_TAGS := optional

LOCAL_C_INCLUDES := external/alsa-lib/include hardware/intel/linux-2.6/include/sound \
                    frameworks/base/include/ \
                    external/tinycompress/include/tinycompress \
                    external/tinycompress/include/sound


include $(BUILD_SHARED_LIBRARY)

# The stub audio policy HAL module that can be used as a skeleton for
# new implementations.
#include $(CLEAR_VARS)

#LOCAL_MODULE := audio_policy.stub
#LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
#LOCAL_SRC_FILES := audio_policy.c
#LOCAL_SHARED_LIBRARIES := liblog libcutils
#LOCAL_MODULE_TAGS := optional

#include $(BUILD_SHARED_LIBRARY)
