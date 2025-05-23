/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef __ANDROID_PLAYER_BASE_H__
#define __ANDROID_PLAYER_BASE_H__

#include <audiomanager/AudioManager.h>
#include <audiomanager/IAudioManager.h>
#include <utils/Mutex.h>

#include "android/media/BnPlayer.h"
#include "media/AudioContainers.h"

namespace android {

class PlayerBase : public ::android::media::BnPlayer
{
public:
    explicit PlayerBase();
    virtual ~PlayerBase() override;

    virtual void destroy() = 0;

    //IPlayer implementation
    virtual binder::Status start() override;
    virtual binder::Status pause() override;
    virtual binder::Status stop() override;
    virtual binder::Status setVolume(float vol) override;
    virtual binder::Status setPan(float pan) override;
    virtual binder::Status setStartDelayMs(int32_t delayMs) override;
    virtual binder::Status applyVolumeShaper(
            const media::VolumeShaperConfiguration& configuration,
            const media::VolumeShaperOperation& operation) override;

            status_t startWithStatus(const DeviceIdVector& deviceIds);
            status_t pauseWithStatus();
            status_t stopWithStatus();

            //FIXME temporary method while some player state is outside of this class
            void reportEvent(player_state_t event, const DeviceIdVector& deviceIds);

            void baseUpdateDeviceIds(const DeviceIdVector& deviceIds);

            /**
             * Updates the mapping in the AudioService between portId and piid
             */
            void triggerPortIdUpdate(audio_port_handle_t portId) const;
protected:

            void init(player_type_t playerType, audio_usage_t usage, audio_session_t sessionId);
            void baseDestroy();

    //IPlayer methods handlers for derived classes
    virtual status_t playerStart()  { return NO_ERROR; }
    virtual status_t playerPause()  { return NO_ERROR; }
    virtual status_t playerStop()  { return NO_ERROR; }
    virtual status_t playerSetVolume()  { return NO_ERROR; }

    // mutex for IPlayer volume and pan, and player-specific volume
    Mutex mSettingsLock;

    // volume multipliers coming from the IPlayer volume and pan controls
    float mPanMultiplierL, mPanMultiplierR;
    float mVolumeMultiplierL, mVolumeMultiplierR;

    // player interface ID, uniquely identifies the player in the system
    // effectively const after PlayerBase::init().
    audio_unique_id_t mPIId;
private:
            // report events to AudioService
            void servicePlayerEvent(player_state_t event, const DeviceIdVector& deviceIds);
            void serviceReleasePlayer();

    // native interface to AudioService
    android::sp<android::IAudioManager> mAudioManager;

    // Mutex for state reporting
    Mutex mPlayerStateLock;
    player_state_t mLastReportedEvent;

    Mutex mDeviceIdLock;
    DeviceIdVector mLastReportedDeviceIds GUARDED_BY(mDeviceIdLock);
};

} // namespace android

#endif /* __ANDROID_PLAYER_BASE_H__ */
