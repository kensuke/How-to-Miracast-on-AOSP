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

#include "Parameters.h"

#include <media/stagefright/MediaErrors.h>

namespace android {

// static
sp<Parameters> Parameters::Parse(const char *data, size_t size) {
    sp<Parameters> params = new Parameters;
    status_t err = params->parse(data, size);

    if (err != OK) {
        return NULL;
    }

    return params;
}

Parameters::Parameters() {}

Parameters::~Parameters() {}

status_t Parameters::parse(const char *data, size_t size) {
    size_t i = 0;
    while (i < size) {
        size_t nameStart = i;
        while (i < size && data[i] != ':') {
            ++i;
        }

        if (i == size || i == nameStart) {
            return ERROR_MALFORMED;
        }

        AString name(&data[nameStart], i - nameStart);
        name.trim();
        name.tolower();

        ++i;

        size_t valueStart = i;

        while (i + 1 < size && (data[i] != '\r' || data[i + 1] != '\n')) {
            ++i;
        }

        AString value(&data[valueStart], i - valueStart);
        value.trim();

        mDict.add(name, value);

        while (i + 1 < size && data[i] == '\r' && data[i + 1] == '\n') {
            i += 2;
        }
    }

    return OK;
}

bool Parameters::findParameter(const char *name, AString *value) const {
    AString key = name;
    key.tolower();

    ssize_t index = mDict.indexOfKey(key);

    if (index < 0) {
        value->clear();

        return false;
    }

    *value = mDict.valueAt(index);
    return true;
}

}  // namespace android
