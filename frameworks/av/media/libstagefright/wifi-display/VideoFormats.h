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

#ifndef VIDEO_FORMATS_H_

#define VIDEO_FORMATS_H_

#include <media/stagefright/foundation/ABase.h>

#include <stdint.h>

namespace android {

struct AString;

// This class encapsulates that video resolution capabilities of a wfd source
// or sink as outlined in the wfd specs. Currently three sets of resolutions
// are specified, each of which supports up to 32 resolutions.
// In addition to its capabilities each sink/source also publishes its
// "native" resolution, presumably one that is preferred among all others
// because it wouldn't require any scaling and directly corresponds to the
// display capabilities/pixels.
struct VideoFormats {
    VideoFormats();

    struct config_t {
        size_t width, height, framesPerSecond;
        bool interlaced;
        unsigned char profile, level;
    };

    enum ProfileType {
        PROFILE_CBP = 0,
        PROFILE_CHP,
        kNumProfileTypes,
    };

    enum LevelType {
        LEVEL_31 = 0,
        LEVEL_32,
        LEVEL_40,
        LEVEL_41,
        LEVEL_42,
        kNumLevelTypes,
    };

    enum ResolutionType {
        RESOLUTION_CEA,
        RESOLUTION_VESA,
        RESOLUTION_HH,
        kNumResolutionTypes,
    };

    void setNativeResolution(ResolutionType type, size_t index);
    void getNativeResolution(ResolutionType *type, size_t *index) const;

    void disableAll();
    void enableAll();
    void enableResolutionUpto(
            ResolutionType type, size_t index,
            ProfileType profile, LevelType level);

    void setResolutionEnabled(
            ResolutionType type, size_t index, bool enabled = true);

    bool isResolutionEnabled(ResolutionType type, size_t index) const;

    void setProfileLevel(
            ResolutionType type, size_t index,
            ProfileType profile, LevelType level);

    void getProfileLevel(
            ResolutionType type, size_t index,
            ProfileType *profile, LevelType *level) const;

    static bool GetConfiguration(
            ResolutionType type, size_t index,
            size_t *width, size_t *height, size_t *framesPerSecond,
            bool *interlaced);

    static bool GetProfileLevel(
            ProfileType profile, LevelType level,
            unsigned *profileIdc, unsigned *levelIdc,
            unsigned *constraintSet);

    bool parseFormatSpec(const char *spec);
    AString getFormatSpec(bool forM4Message = false) const;

    static bool PickBestFormat(
            const VideoFormats &sinkSupported,
            const VideoFormats &sourceSupported,
            ResolutionType *chosenType,
            size_t *chosenIndex,
            ProfileType *chosenProfile,
            LevelType *chosenLevel);

private:
    bool parseH264Codec(const char *spec);
    ResolutionType mNativeType;
    size_t mNativeIndex;

    uint32_t mResolutionEnabled[kNumResolutionTypes];
    static const config_t mResolutionTable[kNumResolutionTypes][32];
    config_t mConfigs[kNumResolutionTypes][32];

    DISALLOW_EVIL_CONSTRUCTORS(VideoFormats);
};

}  // namespace android

#endif  // VIDEO_FORMATS_H_

