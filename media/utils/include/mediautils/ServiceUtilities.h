/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_MEDIAUTILS_SERVICEUTILITIES_H
#define ANDROID_MEDIAUTILS_SERVICEUTILITIES_H

#include <unistd.h>

#include <android/content/pm/IPackageManagerNative.h>
#include <android-base/thread_annotations.h>
#include <binder/IMemory.h>
#include <binder/PermissionController.h>
#include <cutils/multiuser.h>
#include <private/android_filesystem_config.h>
#include <system/audio-hal-enums.h>
#include <android/content/AttributionSourceState.h>
#include <binder/PermissionController.h>
#include <android/permission/PermissionChecker.h>

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace android {

using content::AttributionSourceState;

// Audio permission utilities

// Used for calls that should originate from system services.
// We allow that some services might have separate processes to
// handle multiple users, e.g. u10_system, u10_bluetooth, u10_radio.
static inline bool isServiceUid(uid_t uid) {
    return multiuser_get_app_id(uid) < AID_APP_START;
}

// Used for calls that should originate from audioserver.
static inline bool isAudioServerUid(uid_t uid) {
    return uid == AID_AUDIOSERVER;
}

// Used for some permission checks.
// AID_ROOT is OK for command-line tests.  Native audioserver always OK.
static inline bool isAudioServerOrRootUid(uid_t uid) {
    return uid == AID_AUDIOSERVER || uid == AID_ROOT;
}

// Used for calls that should come from system server or internal.
// Note: system server is multiprocess for multiple users.  audioserver is not.
// Note: if this method is modified, also update the same method in SensorService.h.
static inline bool isAudioServerOrSystemServerUid(uid_t uid) {
    return multiuser_get_app_id(uid) == AID_SYSTEM || uid == AID_AUDIOSERVER;
}

// used for calls that should come from system_server or audio_server or media server and
// include AID_ROOT for command-line tests.
static inline bool isAudioServerOrMediaServerOrSystemServerOrRootUid(uid_t uid) {
    return multiuser_get_app_id(uid) == AID_SYSTEM || uid == AID_AUDIOSERVER
              || uid == AID_MEDIA || uid == AID_ROOT;
}

// Mediaserver may forward the client PID and UID as part of a binder interface call;
// otherwise the calling UID must be equal to the client UID.
static inline bool isAudioServerOrMediaServerUid(uid_t uid) {
    switch (uid) {
    case AID_MEDIA:
    case AID_AUDIOSERVER:
        return true;
    default:
        return false;
    }
}

bool recordingAllowed(const AttributionSourceState& attributionSource,
        audio_source_t source = AUDIO_SOURCE_DEFAULT);

bool recordingAllowed(const AttributionSourceState &attributionSource,
                      uint32_t virtualDeviceId,
                      audio_source_t source);
int startRecording(const AttributionSourceState& attributionSource, uint32_t virtualDeviceId,
                    const String16& msg, audio_source_t source);
void finishRecording(const AttributionSourceState& attributionSource, uint32_t virtualDeviceId,
                     audio_source_t source);
std::optional<AttributionSourceState> resolveAttributionSource(
    const AttributionSourceState& callerAttributionSource, uint32_t virtualDeviceId);
bool captureAudioOutputAllowed(const AttributionSourceState& attributionSource);
bool captureMediaOutputAllowed(const AttributionSourceState& attributionSource);
bool captureTunerAudioInputAllowed(const AttributionSourceState& attributionSource);
bool captureVoiceCommunicationOutputAllowed(const AttributionSourceState& attributionSource);
bool bypassConcurrentPolicyAllowed(const AttributionSourceState& attributionSource) ;
bool accessUltrasoundAllowed(const AttributionSourceState& attributionSource);
bool captureHotwordAllowed(const AttributionSourceState& attributionSource);
bool settingsAllowed();
bool modifyAudioRoutingAllowed();
bool modifyAudioRoutingAllowed(const AttributionSourceState& attributionSource);
bool modifyDefaultAudioEffectsAllowed();
bool modifyDefaultAudioEffectsAllowed(const AttributionSourceState& attributionSource);
bool modifyAudioSettingsPrivilegedAllowed(const AttributionSourceState& attributionSource);
bool dumpAllowed();
bool modifyPhoneStateAllowed(const AttributionSourceState& attributionSource);
bool bypassInterruptionPolicyAllowed(const AttributionSourceState& attributionSource);
bool callAudioInterceptionAllowed(const AttributionSourceState& attributionSource);
void purgePermissionCache();
bool mustAnonymizeBluetoothAddressLegacy(
        const AttributionSourceState& attributionSource, const String16& caller);
void anonymizeBluetoothAddress(char *address);

bool isRecordOpRequired(audio_source_t source);
int32_t getOpForSource(audio_source_t source);

AttributionSourceState getCallingAttributionSource();

status_t checkIMemory(const sp<IMemory>& iMemory);

class MediaPackageManager {
public:
    /** Query the PackageManager to check if all apps of an UID allow playback capture. */
    bool allowPlaybackCapture(uid_t uid) {
        auto result = doIsAllowed(uid);
        if (!result) {
            mPackageManagerErrors++;
        }
        return result.value_or(false);
    }
    void dump(int fd, int spaces = 0) const;
private:
    static constexpr const char* nativePackageManagerName = "package_native";
    std::optional<bool> doIsAllowed(uid_t uid);
    sp<content::pm::IPackageManagerNative> retrievePackageManager();
    sp<content::pm::IPackageManagerNative> mPackageManager; // To check apps manifest
    unsigned int mPackageManagerErrors = 0;
    struct Package {
        std::string name;
        bool playbackCaptureAllowed = false;
    };
    using Packages = std::vector<Package>;
    std::map<uid_t, Packages> mDebugLog;
};

namespace mediautils {

/**
 * This class is used to retrieve (and cache) package information
 * for a given uid.
 */
class UidInfo {
public:
    struct Info {
        uid_t uid = -1;           // uid used for lookup.
        std::string package;      // package name.
        std::string installer;    // installer for the package (e.g. preload, play store).
        int64_t versionCode = 0;  // reported version code.
        int64_t expirationNs = 0; // after this time in SYSTEM_TIME_REALTIME we refetch.
    };

    /**
     * Returns the package information for a UID.
     *
     * The package name will be the uid if we cannot find the associated name.
     *
     * \param uid is the uid of the app or service.
     */
    std::shared_ptr<const Info> getCachedInfo(uid_t uid);

    /* return a singleton */
    static UidInfo& getUidInfo();

    /* returns a non-null pointer to a const Info struct */
    static std::shared_ptr<const Info> getInfo(uid_t uid);

private:
    std::mutex mLock;
    // TODO: use concurrent hashmap with striped lock.
    std::unordered_map<uid_t, std::shared_ptr<const Info>> mInfoMap GUARDED_BY(mLock);
};

} // namespace mediautils

} // namespace android

#endif // ANDROID_MEDIAUTILS_SERVICEUTILITIES_H
