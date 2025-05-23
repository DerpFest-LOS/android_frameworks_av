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

#include <media/TrackPlayerBase.h>

namespace android {
using aidl_utils::binderStatusFromStatusT;
using media::VolumeShaper;

//--------------------------------------------------------------------------------------------------
TrackPlayerBase::TrackPlayerBase() : PlayerBase(),
        mPlayerVolumeL(1.0f), mPlayerVolumeR(1.0f)
{
    ALOGD("TrackPlayerBase::TrackPlayerBase()");
}


TrackPlayerBase::~TrackPlayerBase() {
    ALOGD("TrackPlayerBase::~TrackPlayerBase()");
    doDestroy();
}

void TrackPlayerBase::init(const sp<AudioTrack>& pat,
                           const sp<AudioTrack::IAudioTrackCallback>& callback,
                           player_type_t playerType, audio_usage_t usage,
                           audio_session_t sessionId) {
    PlayerBase::init(playerType, usage, sessionId);
    mAudioTrack.store(pat);
    if (pat != 0) {
        mCallbackHandle = callback;
        mSelfAudioDeviceCallback = new SelfAudioDeviceCallback(*this);
        pat->addAudioDeviceCallback(mSelfAudioDeviceCallback);
        pat->setPlayerIId(mPIId);  // set in PlayerBase::init().
    }
}

void TrackPlayerBase::destroy() {
    doDestroy();
    baseDestroy();
}

TrackPlayerBase::SelfAudioDeviceCallback::SelfAudioDeviceCallback(PlayerBase& self) :
    AudioSystem::AudioDeviceCallback(), mSelf(self) {
}

TrackPlayerBase::SelfAudioDeviceCallback::~SelfAudioDeviceCallback() {
}

void TrackPlayerBase::SelfAudioDeviceCallback::onAudioDeviceUpdate(audio_io_handle_t __unused,
        const DeviceIdVector& deviceIds) {
    mSelf.baseUpdateDeviceIds(deviceIds);
}

void TrackPlayerBase::doDestroy() {
    sp<AudioTrack> audioTrack = getAudioTrack();

    // Note that there may still be another reference in post-unlock phase of SetPlayState
    clearAudioTrack();

    if (audioTrack != 0) {
        audioTrack->stop();
        audioTrack->removeAudioDeviceCallback(mSelfAudioDeviceCallback);
        mSelfAudioDeviceCallback.clear();
    }
}

void TrackPlayerBase::setPlayerVolume(float vl, float vr) {
    {
        Mutex::Autolock _l(mSettingsLock);
        mPlayerVolumeL = vl;
        mPlayerVolumeR = vr;
    }
    doSetVolume();
}

//------------------------------------------------------------------------------
// Implementation of IPlayer
status_t TrackPlayerBase::playerStart() {
    status_t status = NO_INIT;
    if (sp<AudioTrack> audioTrack = getAudioTrack(); audioTrack != 0) {
        status = audioTrack->start();
    }
    return status;
}

status_t TrackPlayerBase::playerPause() {
    status_t status = NO_INIT;
    if (sp<AudioTrack> audioTrack = getAudioTrack(); audioTrack != 0) {
        audioTrack->pause();
        status = NO_ERROR;
    }
    return status;
}


status_t TrackPlayerBase::playerStop() {
    status_t status = NO_INIT;
    if (sp<AudioTrack> audioTrack = getAudioTrack(); audioTrack != 0) {
        audioTrack->stop();
        status = NO_ERROR;
    }
    return status;
}

status_t TrackPlayerBase::playerSetVolume() {
    return doSetVolume();
}

status_t TrackPlayerBase::doSetVolume() {
    status_t status = NO_INIT;
    if (sp<AudioTrack> audioTrack = getAudioTrack(); audioTrack != 0) {
        float tl = mPlayerVolumeL * mPanMultiplierL * mVolumeMultiplierL;
        float tr = mPlayerVolumeR * mPanMultiplierR * mVolumeMultiplierR;
        audioTrack->setVolume(tl, tr);
        status = NO_ERROR;
    }
    return status;
}


binder::Status TrackPlayerBase::applyVolumeShaper(
        const media::VolumeShaperConfiguration& configuration,
        const media::VolumeShaperOperation& operation) {

    sp<VolumeShaper::Configuration> spConfiguration = new VolumeShaper::Configuration();
    sp<VolumeShaper::Operation> spOperation = new VolumeShaper::Operation();

    status_t s = spConfiguration->readFromParcelable(configuration)
            ?: spOperation->readFromParcelable(operation);
    if (s != OK) {
        return binderStatusFromStatusT(s);
    }
    if (sp<AudioTrack> audioTrack = getAudioTrack(); audioTrack != 0) {
        ALOGD("TrackPlayerBase::applyVolumeShaper() from IPlayer");
        VolumeShaper::Status status = audioTrack->applyVolumeShaper(spConfiguration, spOperation);
        if (status < 0) { // a non-negative value is the volume shaper id.
            ALOGE("TrackPlayerBase::applyVolumeShaper() failed with status %d", status);
        }
        return binderStatusFromStatusT(status);
    } else {
        ALOGD("TrackPlayerBase::applyVolumeShaper()"
              " no AudioTrack for volume control from IPlayer");
        return binder::Status::ok();
    }
}

} // namespace android
