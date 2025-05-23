/*
 * Copyright (C) 2024 The Android Open Source Project
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

#pragma once

#include <system/audio.h>

namespace android {

class VolumePortInterface : public virtual RefBase {
public:
    virtual void setPortVolume(float volume) = 0;
    virtual void setPortMute(bool mute) = 0;
    virtual float getPortVolume() const = 0;
    /** Returns the muted state defined by the volume group which is playing on this port. */
    virtual bool getPortMute() const = 0;
};

}  // namespace android
