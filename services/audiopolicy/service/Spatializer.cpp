/*
**
** Copyright 2021, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <string>
#define LOG_TAG "Spatializer"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <algorithm>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#include <android/content/AttributionSourceState.h>
#include <android/sysprop/BluetoothProperties.sysprop.h>
#include <audio_utils/fixedfft.h>
#include <cutils/bitops.h>
#include <hardware/sensors.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/MediaMetricsItem.h>
#include <media/QuaternionUtil.h>
#include <media/ShmemCompat.h>
#include <mediautils/SchedulingPolicyService.h>
#include <mediautils/ServiceUtilities.h>
#include <utils/Thread.h>

#include "Spatializer.h"
#include "SpatializerHelper.h"

namespace android {

using aidl_utils::binderStatusFromStatusT;
using aidl_utils::statusTFromBinderStatus;
using android::content::AttributionSourceState;
using binder::Status;
using internal::ToString;
using media::HeadTrackingMode;
using media::Pose3f;
using media::SensorPoseProvider;
using media::audio::common::HeadTracking;
using media::audio::common::Spatialization;

using namespace std::chrono_literals;

#define VALUE_OR_RETURN_BINDER_STATUS(x) \
    ({ auto _tmp = (x); \
       if (!_tmp.ok()) return aidl_utils::binderStatusFromStatusT(_tmp.error()); \
       std::move(_tmp.value()); })

static audio_channel_mask_t getMaxChannelMask(
        const std::vector<audio_channel_mask_t>& masks, size_t channelLimit = SIZE_MAX) {
    uint32_t maxCount = 0;
    audio_channel_mask_t maxMask = AUDIO_CHANNEL_NONE;
    for (auto mask : masks) {
        const size_t count = audio_channel_count_from_out_mask(mask);
        if (count > channelLimit) continue;  // ignore masks greater than channelLimit
        if (count > maxCount) {
            maxMask = mask;
            maxCount = count;
        }
    }
    return maxMask;
}

static std::vector<float> recordFromTranslationRotationVector(
        const std::vector<float>& trVector) {
    auto headToStageOpt = Pose3f::fromVector(trVector);
    if (!headToStageOpt) return {};

    const auto stageToHead = headToStageOpt.value().inverse();
    const auto stageToHeadTranslation = stageToHead.translation();
    constexpr float RAD_TO_DEGREE = 180.f / M_PI;
    std::vector<float> record{
        stageToHeadTranslation[0], stageToHeadTranslation[1], stageToHeadTranslation[2],
        0.f, 0.f, 0.f};
    media::quaternionToAngles(stageToHead.rotation(), &record[3], &record[4], &record[5]);
    record[3] *= RAD_TO_DEGREE;
    record[4] *= RAD_TO_DEGREE;
    record[5] *= RAD_TO_DEGREE;
    return record;
}

template<typename T>
static constexpr const T& safe_clamp(const T& value, const T& low, const T& high) {
    if constexpr (std::is_floating_point_v<T>) {
        return value != value /* constexpr isnan */
                ? low : std::clamp(value, low, high);
    } else /* constexpr */ {
        return std::clamp(value, low, high);
    }
}

// ---------------------------------------------------------------------------

class Spatializer::EngineCallbackHandler : public AHandler {
public:
    EngineCallbackHandler(wp<Spatializer> spatializer)
            : mSpatializer(spatializer) {
    }

    enum {
        // Device state callbacks
        kWhatOnFramesProcessed,    // AudioEffect::EVENT_FRAMES_PROCESSED
        kWhatOnHeadToStagePose,    // SpatializerPoseController::Listener::onHeadToStagePose
        kWhatOnActualModeChange,   // SpatializerPoseController::Listener::onActualModeChange
        kWhatOnLatencyModesChanged, // Spatializer::onSupportedLatencyModesChanged
    };
    static constexpr const char *kNumFramesKey = "numFrames";
    static constexpr const char *kModeKey = "mode";
    static constexpr const char *kTranslation0Key = "translation0";
    static constexpr const char *kTranslation1Key = "translation1";
    static constexpr const char *kTranslation2Key = "translation2";
    static constexpr const char *kRotation0Key = "rotation0";
    static constexpr const char *kRotation1Key = "rotation1";
    static constexpr const char *kRotation2Key = "rotation2";
    static constexpr const char *kLatencyModesKey = "latencyModes";

    class LatencyModes : public RefBase {
    public:
        LatencyModes(audio_io_handle_t output,
                const std::vector<audio_latency_mode_t>& latencyModes)
            : mOutput(output), mLatencyModes(latencyModes) {}
        ~LatencyModes() = default;

        audio_io_handle_t mOutput;
        std::vector<audio_latency_mode_t> mLatencyModes;
    };

    void onMessageReceived(const sp<AMessage> &msg) override {
        // No ALooper method to get the tid so update
        // Spatializer priority on the first message received.
        std::call_once(mPrioritySetFlag, [](){
            const pid_t pid = getpid();
            const pid_t tid = gettid();
            (void)requestSpatializerPriority(pid, tid);
        });

        sp<Spatializer> spatializer = mSpatializer.promote();
        if (spatializer == nullptr) {
            ALOGW("%s: Cannot promote spatializer", __func__);
            return;
        }
        switch (msg->what()) {
            case kWhatOnFramesProcessed: {
                int numFrames;
                if (!msg->findInt32(kNumFramesKey, &numFrames)) {
                    ALOGE("%s: Cannot find num frames!", __func__);
                    return;
                }
                if (numFrames > 0) {
                    spatializer->calculateHeadPose();
                }
                } break;
            case kWhatOnHeadToStagePose: {
                std::vector<float> headToStage(sHeadPoseKeys.size());
                for (size_t i = 0 ; i < sHeadPoseKeys.size(); i++) {
                    if (!msg->findFloat(sHeadPoseKeys[i], &headToStage[i])) {
                        ALOGE("%s: Cannot find kTranslation0Key!", __func__);
                        return;
                    }
                }
                spatializer->onHeadToStagePoseMsg(headToStage);
                } break;
            case kWhatOnActualModeChange: {
                int mode;
                if (!msg->findInt32(kModeKey, &mode)) {
                    ALOGE("%s: Cannot find actualMode!", __func__);
                    return;
                }
                spatializer->onActualModeChangeMsg(static_cast<HeadTrackingMode>(mode));
                } break;

            case kWhatOnLatencyModesChanged: {
                sp<RefBase> object;
                if (!msg->findObject(kLatencyModesKey, &object)) {
                    ALOGE("%s: Cannot find latency modes!", __func__);
                    return;
                }
                sp<LatencyModes> latencyModes = static_cast<LatencyModes*>(object.get());
                spatializer->onSupportedLatencyModesChangedMsg(
                    latencyModes->mOutput, std::move(latencyModes->mLatencyModes));
                } break;

            default:
                LOG_ALWAYS_FATAL("Invalid callback message %d", msg->what());
        }
    }
private:
    wp<Spatializer> mSpatializer;
    std::once_flag mPrioritySetFlag;
};

const std::vector<const char *> Spatializer::sHeadPoseKeys = {
    Spatializer::EngineCallbackHandler::kTranslation0Key,
    Spatializer::EngineCallbackHandler::kTranslation1Key,
    Spatializer::EngineCallbackHandler::kTranslation2Key,
    Spatializer::EngineCallbackHandler::kRotation0Key,
    Spatializer::EngineCallbackHandler::kRotation1Key,
    Spatializer::EngineCallbackHandler::kRotation2Key,
};

// Mapping table between strings read form property bluetooth.core.le.dsa_transport_preference
// and low latency modes emums.
//TODO b/273373363: use AIDL enum when available
const std::map<std::string, audio_latency_mode_t> Spatializer::sStringToLatencyModeMap = {
    {"le-acl", AUDIO_LATENCY_MODE_LOW},
    {"iso-sw", AUDIO_LATENCY_MODE_DYNAMIC_SPATIAL_AUDIO_SOFTWARE},
    {"iso-hw", AUDIO_LATENCY_MODE_DYNAMIC_SPATIAL_AUDIO_HARDWARE},
};

void Spatializer::loadOrderedLowLatencyModes() {
    if (!com::android::media::audio::dsa_over_bt_le_audio()) {
        return;
    }
    auto latencyModesStrs = android::sysprop::BluetoothProperties::dsa_transport_preference();
    audio_utils::lock_guard lock(mMutex);
    // First load preferred low latency modes ordered from the property
    for (auto str : latencyModesStrs) {
        if (!str.has_value()) continue;
        if (auto it = sStringToLatencyModeMap.find(str.value());
                it != sStringToLatencyModeMap.end()) {
            mOrderedLowLatencyModes.push_back(it->second);
        }
    }
    // Then add unlisted latency modes at the end of the ordered list
    for (auto it : sStringToLatencyModeMap) {
        if (std::find(mOrderedLowLatencyModes.begin(), mOrderedLowLatencyModes.end(), it.second)
                == mOrderedLowLatencyModes.end()) {
             mOrderedLowLatencyModes.push_back(it.second);
        }
    }
}

// ---------------------------------------------------------------------------
sp<Spatializer> Spatializer::create(SpatializerPolicyCallback* callback,
                                    const sp<EffectsFactoryHalInterface>& effectsFactoryHal) {
    sp<Spatializer> spatializer;

    if (effectsFactoryHal == nullptr) {
        ALOGW("%s failed to create effect factory interface", __func__);
        return spatializer;
    }

    std::vector<effect_descriptor_t> descriptors;
    status_t status = effectsFactoryHal->getDescriptors(FX_IID_SPATIALIZER, &descriptors);
    if (status != NO_ERROR) {
        ALOGW("%s failed to get spatializer descriptor, error %d", __func__, status);
        return spatializer;
    }
    ALOG_ASSERT(!descriptors.empty(),
            "%s getDescriptors() returned no error but empty list", __func__);

    // TODO: get supported spatialization modes from FX engine or descriptor
    sp<EffectHalInterface> effect;
    status = effectsFactoryHal->createEffect(&descriptors[0].uuid, AUDIO_SESSION_OUTPUT_STAGE,
            AUDIO_IO_HANDLE_NONE, AUDIO_PORT_HANDLE_NONE, &effect);
    ALOGI("%s FX create status %d effect %p", __func__, status, effect.get());

    if (status == NO_ERROR && effect != nullptr) {
        spatializer = new Spatializer(descriptors[0], callback);
        if (spatializer->loadEngineConfiguration(effect) != NO_ERROR) {
            spatializer.clear();
            ALOGW("%s loadEngine error: %d  effect %p", __func__, status, effect.get());
        } else {
            spatializer->loadOrderedLowLatencyModes();
            spatializer->mLocalLog.log("%s with effect Id %p", __func__, effect.get());
        }
    }

    return spatializer;
}

Spatializer::Spatializer(effect_descriptor_t engineDescriptor, SpatializerPolicyCallback* callback)
    : mEngineDescriptor(engineDescriptor),
      mPolicyCallback(callback) {
    ALOGV("%s", __func__);
    setMinSchedulerPolicy(SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
    setInheritRt(true);
}

void Spatializer::onFirstRef() {
    mLooper = new ALooper;
    mLooper->setName("Spatializer-looper");
    mLooper->start(
            /*runOnCallingThread*/false,
            /*canCallJava*/       false,
            PRIORITY_URGENT_AUDIO);

    mHandler = new EngineCallbackHandler(this);
    mLooper->registerHandler(mHandler);
}

Spatializer::~Spatializer() {
    ALOGV("%s", __func__);
    if (mLooper != nullptr) {
        mLooper->stop();
        mLooper->unregisterHandler(mHandler->id());
    }
    mLooper.clear();
    mHandler.clear();
}

static std::string channelMaskVectorToString(
        const std::vector<audio_channel_mask_t>& masks) {
    std::stringstream ss;
    for (const auto &mask : masks) {
        if (ss.tellp() != 0) ss << "|";
        ss << mask;
    }
    return ss.str();
}

status_t Spatializer::loadEngineConfiguration(sp<EffectHalInterface> effect) {
    ALOGV("%s", __func__);

    std::vector<bool> supportsHeadTracking;
    status_t status = getHalParameter<false>(effect, SPATIALIZER_PARAM_HEADTRACKING_SUPPORTED,
                                         &supportsHeadTracking);
    if (status != NO_ERROR) {
        ALOGW("%s: cannot get SPATIALIZER_PARAM_HEADTRACKING_SUPPORTED", __func__);
        return status;
    }
    mSupportsHeadTracking = supportsHeadTracking[0];

    std::vector<Spatialization::Level> spatializationLevels;
    status = getHalParameter<true>(effect, SPATIALIZER_PARAM_SUPPORTED_LEVELS,
            &spatializationLevels);
    if (status != NO_ERROR) {
        ALOGW("%s: cannot get SPATIALIZER_PARAM_SUPPORTED_LEVELS", __func__);
        return status;
    }
    bool noneLevelFound = false;
    bool activeLevelFound = false;
    for (const auto spatializationLevel : spatializationLevels) {
        if (!aidl_utils::isValidEnum(spatializationLevel)) {
            ALOGW("%s: ignoring spatializationLevel:%s", __func__,
                  ToString(spatializationLevel).c_str());
            continue;
        }
        if (spatializationLevel == Spatialization::Level::NONE) {
            noneLevelFound = true;
        } else {
            activeLevelFound = true;
        }
        // we don't detect duplicates.
        mLevels.emplace_back(spatializationLevel);
    }
    if (!noneLevelFound || !activeLevelFound) {
        ALOGW("%s: SPATIALIZER_PARAM_SUPPORTED_LEVELS must include NONE"
                " and another valid level",  __func__);
        return BAD_VALUE;
    }

    std::vector<Spatialization::Mode> spatializationModes;
    status = getHalParameter<true>(effect, SPATIALIZER_PARAM_SUPPORTED_SPATIALIZATION_MODES,
            &spatializationModes);
    if (status != NO_ERROR) {
        ALOGW("%s: cannot get SPATIALIZER_PARAM_SUPPORTED_SPATIALIZATION_MODES", __func__);
        return status;
    }

    for (const auto spatializationMode : spatializationModes) {
        if (!aidl_utils::isValidEnum(spatializationMode)) {
            ALOGW("%s: ignoring spatializationMode:%s", __func__,
                  ToString(spatializationMode).c_str());
            continue;
        }
        // we don't detect duplicates.
        mSpatializationModes.emplace_back(spatializationMode);
    }
    if (mSpatializationModes.empty()) {
        ALOGW("%s: SPATIALIZER_PARAM_SUPPORTED_SPATIALIZATION_MODES reports empty", __func__);
        return BAD_VALUE;
    }

    std::vector<audio_channel_mask_t> channelMasks;
    status = getHalParameter<true>(effect, SPATIALIZER_PARAM_SUPPORTED_CHANNEL_MASKS,
                                 &channelMasks);
    if (status != NO_ERROR) {
        ALOGW("%s: cannot get SPATIALIZER_PARAM_SUPPORTED_CHANNEL_MASKS", __func__);
        return status;
    }
    for (const auto channelMask : channelMasks) {
        const bool channel_mask_spatialized =
                SpatializerHelper::isStereoSpatializationFeatureEnabled()
                        ? audio_channel_mask_contains_stereo(channelMask)
                        : audio_is_channel_mask_spatialized(channelMask);
        if (!channel_mask_spatialized) {
            ALOGW("%s: ignoring channelMask:%#x", __func__, channelMask);
            continue;
        }
        // we don't detect duplicates.
        mChannelMasks.emplace_back(channelMask);
    }
    if (mChannelMasks.empty()) {
        ALOGW("%s: SPATIALIZER_PARAM_SUPPORTED_CHANNEL_MASKS reports empty", __func__);
        return BAD_VALUE;
    }

    std::vector<audio_channel_mask_t> spatializedChannelMasks;
    status = getHalParameter<true>(effect, SPATIALIZER_PARAM_SPATIALIZED_CHANNEL_MASKS,
                                   &spatializedChannelMasks);
    if (status != NO_ERROR) {
        ALOGW("%s: cannot get SPATIALIZER_PARAM_SPATIALIZED_CHANNEL_MASKS", __func__);
        // do not return an error yet as spatializer implementations may not have been
        // updated yet to support this parameter
    }
    if (spatializedChannelMasks.empty()) {
        ALOGW("%s: SPATIALIZER_PARAM_SPATIALIZED_CHANNEL_MASKS reports empty", __func__);
        // do not return an error yet as spatializer implementations may not have been
        // updated yet to support this parameter
    }
    for (const audio_channel_mask_t spatializedMask : spatializedChannelMasks) {
        // spatialized masks must be contained in the supported input masks
        if (std::find(mChannelMasks.begin(), mChannelMasks.end(), spatializedMask)
                != mChannelMasks.end()) {
            mSpatializedChannelMasks.emplace_back(spatializedMask);
        } else {
            ALOGE("%s: spatialized mask %#x not in list reported by PARAM_SUPPORTED_CHANNEL_MASKS",
                  __func__, spatializedMask);
            return BAD_VALUE;
        }
    }

    if (com::android::media::audio::dsa_over_bt_le_audio()
            && mSupportsHeadTracking) {
        mHeadtrackingConnectionMode = HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED;
        std::vector<HeadTracking::ConnectionMode> headtrackingConnectionModes;
        status = getHalParameter<true>(effect, SPATIALIZER_PARAM_SUPPORTED_HEADTRACKING_CONNECTION,
                &headtrackingConnectionModes);
        if (status == NO_ERROR) {
            for (const auto htConnectionMode : headtrackingConnectionModes) {
                if (htConnectionMode < HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED ||
                    htConnectionMode > HeadTracking::ConnectionMode::DIRECT_TO_SENSOR_TUNNEL) {
                    ALOGW("%s: ignoring HT connection mode:%s", __func__,
                          ToString(htConnectionMode).c_str());
                    continue;
                }
                mSupportedHeadtrackingConnectionModes.insert(htConnectionMode);
            }
            ALOGW_IF(mSupportedHeadtrackingConnectionModes.find(
                    HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED) ==
                        mSupportedHeadtrackingConnectionModes.end(),
                    "%s: Headtracking FRAMEWORK_PROCESSED not reported", __func__);
        }
    }

    // Currently we expose only RELATIVE_WORLD.
    // This is a limitation of the head tracking library based on a UX choice.
    mHeadTrackingModes.push_back(HeadTracking::Mode::DISABLED);
    if (mSupportsHeadTracking) {
        mHeadTrackingModes.push_back(HeadTracking::Mode::RELATIVE_WORLD);
    }
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE)
        .set(AMEDIAMETRICS_PROP_CHANNELMASKS, channelMaskVectorToString(mChannelMasks))
        .set(AMEDIAMETRICS_PROP_LEVELS, aidl_utils::enumsToString(mLevels))
        .set(AMEDIAMETRICS_PROP_MODES, aidl_utils::enumsToString(mSpatializationModes))
        .set(AMEDIAMETRICS_PROP_HEADTRACKINGMODES, aidl_utils::enumsToString(mHeadTrackingModes))
        .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)status)
        .record();
    return NO_ERROR;
}

/* static */
void Spatializer::sendEmptyCreateSpatializerMetricWithStatus(status_t status) {
    mediametrics::LogItem(kDefaultMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE)
        .set(AMEDIAMETRICS_PROP_CHANNELMASKS, "")
        .set(AMEDIAMETRICS_PROP_LEVELS, "")
        .set(AMEDIAMETRICS_PROP_MODES, "")
        .set(AMEDIAMETRICS_PROP_HEADTRACKINGMODES, "")
        .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)status)
        .record();
}

/** Gets the channel mask, sampling rate and format set for the spatializer input. */
audio_config_base_t Spatializer::getAudioInConfig() const {
    audio_utils::lock_guard lock(mMutex);
    audio_config_base_t config = AUDIO_CONFIG_BASE_INITIALIZER;
    // For now use highest supported channel count
    config.channel_mask = getMaxChannelMask(mChannelMasks, FCC_LIMIT);
    return config;
}

Status Spatializer::getSpatializedChannelMasks(std::vector<int>* masks) {
        audio_utils::lock_guard lock(mMutex);
        ALOGV("%s", __func__);
        if (masks == nullptr) {
            return binderStatusFromStatusT(BAD_VALUE);
        }
        masks->insert(masks->end(), mSpatializedChannelMasks.begin(),
                      mSpatializedChannelMasks.end());
        return Status::ok();
    }

status_t Spatializer::registerCallback(
        const sp<media::INativeSpatializerCallback>& callback) {
    audio_utils::lock_guard lock(mMutex);
    if (callback == nullptr) {
        return BAD_VALUE;
    }

    if (mSpatializerCallback != nullptr) {
        if (IInterface::asBinder(callback) == IInterface::asBinder(mSpatializerCallback)) {
            ALOGW("%s: Registering callback %p again",
                __func__, mSpatializerCallback.get());
            return NO_ERROR;
        }
        ALOGE("%s: Already one client registered with callback %p",
            __func__, mSpatializerCallback.get());
        return INVALID_OPERATION;
    }

    sp<IBinder> binder = IInterface::asBinder(callback);
    status_t status = binder->linkToDeath(this);
    if (status == NO_ERROR) {
        mSpatializerCallback = callback;
    }
    ALOGV("%s status %d", __func__, status);
    return status;
}

// IBinder::DeathRecipient
void Spatializer::binderDied(__unused const wp<IBinder> &who) {
    {
        audio_utils::lock_guard lock(mMutex);
        mLevel = Spatialization::Level::NONE;
        mSpatializerCallback.clear();
    }
    ALOGV("%s", __func__);
    mPolicyCallback->onCheckSpatializer();
}

// ISpatializer
Status Spatializer::getSupportedLevels(std::vector<Spatialization::Level> *levels) {
    ALOGV("%s", __func__);
    if (levels == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    // Spatialization::Level::NONE is already required from the effect or we don't load it.
    levels->insert(levels->end(), mLevels.begin(), mLevels.end());
    return Status::ok();
}

Status Spatializer::setLevel(Spatialization::Level level) {
    ALOGV("%s level %s", __func__,  ToString(level).c_str());
    mLocalLog.log("%s with %s", __func__, ToString(level).c_str());
    if (level != Spatialization::Level::NONE
            && std::find(mLevels.begin(), mLevels.end(), level) == mLevels.end()) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    sp<media::INativeSpatializerCallback> callback;
    bool levelChanged = false;
    {
        audio_utils::lock_guard lock(mMutex);
        levelChanged = mLevel != level;
        mLevel = level;
        callback = mSpatializerCallback;

        if (levelChanged && mEngine != nullptr) {
            checkEngineState_l();
        }
        checkSensorsState_l();
    }

    if (levelChanged) {
        mPolicyCallback->onCheckSpatializer();
        if (callback != nullptr) {
            callback->onLevelChanged(level);
        }
    }
    return Status::ok();
}

Status Spatializer::getLevel(Spatialization::Level *level) {
    if (level == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    *level = mLevel;
    ALOGV("%s level %s", __func__, ToString(*level).c_str());
    return Status::ok();
}

Status Spatializer::isHeadTrackingSupported(bool *supports) {
    ALOGV("%s mSupportsHeadTracking %s", __func__, ToString(mSupportsHeadTracking).c_str());
    if (supports == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    *supports = mSupportsHeadTracking;
    return Status::ok();
}

Status Spatializer::getSupportedHeadTrackingModes(
        std::vector<HeadTracking::Mode>* modes) {
    audio_utils::lock_guard lock(mMutex);
    ALOGV("%s", __func__);
    if (modes == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    modes->insert(modes->end(), mHeadTrackingModes.begin(), mHeadTrackingModes.end());
    return Status::ok();
}

Status Spatializer::setDesiredHeadTrackingMode(HeadTracking::Mode mode) {
    ALOGV("%s mode %s", __func__, ToString(mode).c_str());

    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    mLocalLog.log("%s with %s", __func__, ToString(mode).c_str());
    audio_utils::lock_guard lock(mMutex);
    switch (mode) {
        case HeadTracking::Mode::OTHER:
            return binderStatusFromStatusT(BAD_VALUE);
        case HeadTracking::Mode::DISABLED:
            mDesiredHeadTrackingMode = HeadTrackingMode::STATIC;
            break;
        case HeadTracking::Mode::RELATIVE_WORLD:
            mDesiredHeadTrackingMode = HeadTrackingMode::WORLD_RELATIVE;
            break;
        case HeadTracking::Mode::RELATIVE_SCREEN:
            mDesiredHeadTrackingMode = HeadTrackingMode::SCREEN_RELATIVE;
            break;
    }

    checkPoseController_l();
    checkSensorsState_l();

    return Status::ok();
}

Status Spatializer::getActualHeadTrackingMode(HeadTracking::Mode *mode) {
    if (mode == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    *mode = mActualHeadTrackingMode;
    ALOGV("%s mode %d", __func__, (int)*mode);
    return Status::ok();
}

Status Spatializer::recenterHeadTracker() {
    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    audio_utils::lock_guard lock(mMutex);
    if (mPoseController != nullptr) {
        mPoseController->recenter();
    }
    return Status::ok();
}

Status Spatializer::setGlobalTransform(const std::vector<float>& screenToStage) {
    ALOGV("%s", __func__);
    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    std::optional<Pose3f> maybePose = Pose3f::fromVector(screenToStage);
    if (!maybePose.has_value()) {
        ALOGW("Invalid screenToStage vector.");
        return binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    if (mPoseController != nullptr) {
        mLocalLog.log("%s with screenToStage %s", __func__,
                media::VectorRecorder::toString<float>(screenToStage).c_str());
        mPoseController->setScreenToStagePose(maybePose.value());
    }
    return Status::ok();
}

Status Spatializer::release() {
    ALOGV("%s", __func__);
    bool levelChanged = false;
    {
        audio_utils::lock_guard lock(mMutex);
        if (mSpatializerCallback == nullptr) {
            return binderStatusFromStatusT(INVALID_OPERATION);
        }

        sp<IBinder> binder = IInterface::asBinder(mSpatializerCallback);
        binder->unlinkToDeath(this);
        mSpatializerCallback.clear();

        levelChanged = mLevel != Spatialization::Level::NONE;
        mLevel = Spatialization::Level::NONE;
    }

    if (levelChanged) {
        mPolicyCallback->onCheckSpatializer();
    }
    return Status::ok();
}

Status Spatializer::setHeadSensor(int sensorHandle) {
    ALOGV("%s sensorHandle %d", __func__, sensorHandle);
    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    audio_utils::lock_guard lock(mMutex);
    if (mHeadSensor != sensorHandle) {
        mLocalLog.log("%s with 0x%08x", __func__, sensorHandle);
        mHeadSensor = sensorHandle;
        checkPoseController_l();
        checkSensorsState_l();
    }
    return Status::ok();
}

Status Spatializer::setScreenSensor(int sensorHandle) {
    ALOGV("%s sensorHandle %d", __func__, sensorHandle);
    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    audio_utils::lock_guard lock(mMutex);
    if (mScreenSensor != sensorHandle) {
        mLocalLog.log("%s with 0x%08x", __func__, sensorHandle);
        mScreenSensor = sensorHandle;
        // TODO: consider a new method setHeadAndScreenSensor()
        // because we generally set both at the same time.
        // This will avoid duplicated work and recentering.
        checkSensorsState_l();
    }
    return Status::ok();
}

Status Spatializer::setDisplayOrientation(float physicalToLogicalAngle) {
    ALOGV("%s physicalToLogicalAngle %f", __func__, physicalToLogicalAngle);
    mLocalLog.log("%s with %f", __func__, physicalToLogicalAngle);
    const float angle = safe_clamp(physicalToLogicalAngle, 0.f, (float)(2. * M_PI));
    // It is possible due to numerical inaccuracies to exceed the boundaries of 0 to 2 * M_PI.
    ALOGI_IF(angle != physicalToLogicalAngle,
            "%s: clamping %f to %f", __func__, physicalToLogicalAngle, angle);
    audio_utils::lock_guard lock(mMutex);
    mDisplayOrientation = angle;
    if (mPoseController != nullptr) {
        // This turns on the rate-limiter.
        mPoseController->setDisplayOrientation(angle);
    }
    if (mEngine != nullptr) {
        setEffectParameter_l(
            SPATIALIZER_PARAM_DISPLAY_ORIENTATION, std::vector<float>{angle});
    }
    return Status::ok();
}

Status Spatializer::setHingeAngle(float hingeAngle) {
    ALOGV("%s hingeAngle %f", __func__, hingeAngle);
    mLocalLog.log("%s with %f", __func__, hingeAngle);
    const float angle = safe_clamp(hingeAngle, 0.f, (float)(2. * M_PI));
    // It is possible due to numerical inaccuracies to exceed the boundaries of 0 to 2 * M_PI.
    ALOGI_IF(angle != hingeAngle,
            "%s: clamping %f to %f", __func__, hingeAngle, angle);
    audio_utils::lock_guard lock(mMutex);
    mHingeAngle = angle;
    if (mEngine != nullptr) {
        setEffectParameter_l(SPATIALIZER_PARAM_HINGE_ANGLE, std::vector<float>{angle});
    }
    return Status::ok();
}

Status Spatializer::setFoldState(bool folded) {
    ALOGV("%s foldState %d", __func__, (int)folded);
    mLocalLog.log("%s with %d", __func__, (int)folded);
    audio_utils::lock_guard lock(mMutex);
    mFoldedState = folded;
    if (mEngine != nullptr) {
        // we don't suppress multiple calls with the same folded state - that's
        // done at the caller.
        setEffectParameter_l(SPATIALIZER_PARAM_FOLD_STATE, std::vector<uint8_t>{mFoldedState});
    }
    return Status::ok();
}

Status Spatializer::getSupportedModes(std::vector<Spatialization::Mode> *modes) {
    ALOGV("%s", __func__);
    if (modes == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    *modes = mSpatializationModes;
    return Status::ok();
}

Status Spatializer::registerHeadTrackingCallback(
        const sp<media::ISpatializerHeadTrackingCallback>& callback) {
    ALOGV("%s callback %p", __func__, callback.get());
    audio_utils::lock_guard lock(mMutex);
    if (!mSupportsHeadTracking) {
        return binderStatusFromStatusT(INVALID_OPERATION);
    }
    mHeadTrackingCallback = callback;
    return Status::ok();
}

Status Spatializer::setParameter(int key, const std::vector<unsigned char>& value) {
    ALOGV("%s key %d", __func__, key);
    audio_utils::lock_guard lock(mMutex);
    status_t status = INVALID_OPERATION;
    if (mEngine != nullptr) {
        status = setEffectParameter_l(key, value);
    }
    return binderStatusFromStatusT(status);
}

Status Spatializer::getParameter(int key, std::vector<unsigned char> *value) {
    ALOGV("%s key %d value size %d", __func__, key,
          (value != nullptr ? (int)value->size() : -1));
    if (value == nullptr) {
        return binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    status_t status = INVALID_OPERATION;
    if (mEngine != nullptr) {
        ALOGV("%s key %d mEngine %p", __func__, key, mEngine.get());
        status = getEffectParameter_l(key, value);
    }
    return binderStatusFromStatusT(status);
}

Status Spatializer::getOutput(int *output) {
    ALOGV("%s", __func__);
    if (output == nullptr) {
        binderStatusFromStatusT(BAD_VALUE);
    }
    audio_utils::lock_guard lock(mMutex);
    *output = VALUE_OR_RETURN_BINDER_STATUS(legacy2aidl_audio_io_handle_t_int32_t(mOutput));
    ALOGV("%s got output %d", __func__, *output);
    return Status::ok();
}

// SpatializerPoseController::Listener
void Spatializer::onHeadToStagePose(const Pose3f& headToStage) {
    ALOGV("%s", __func__);
    LOG_ALWAYS_FATAL_IF(!mSupportsHeadTracking,
            "onHeadToStagePose() called with no head tracking support!");

    auto vec = headToStage.toVector();
    LOG_ALWAYS_FATAL_IF(vec.size() != sHeadPoseKeys.size(),
            "%s invalid head to stage vector size %zu", __func__, vec.size());
    sp<AMessage> msg =
            new AMessage(EngineCallbackHandler::kWhatOnHeadToStagePose, mHandler);
    for (size_t i = 0 ; i < sHeadPoseKeys.size(); i++) {
        msg->setFloat(sHeadPoseKeys[i], vec[i]);
    }
    msg->post();
}

void Spatializer::resetEngineHeadPose_l() {
    ALOGV("%s mEngine %p", __func__, mEngine.get());
    if (mEngine == nullptr) {
        return;
    }
    const std::vector<float> headToStage(6, 0.0);
    setEffectParameter_l(SPATIALIZER_PARAM_HEAD_TO_STAGE, headToStage);
    setEffectParameter_l(SPATIALIZER_PARAM_HEADTRACKING_MODE,
            std::vector<HeadTracking::Mode>{HeadTracking::Mode::DISABLED});
}

void Spatializer::onHeadToStagePoseMsg(const std::vector<float>& headToStage) {
    ALOGV("%s", __func__);
    sp<media::ISpatializerHeadTrackingCallback> callback;
    {
        audio_utils::lock_guard lock(mMutex);
        callback = mHeadTrackingCallback;
        if (mEngine != nullptr) {
            setEffectParameter_l(SPATIALIZER_PARAM_HEAD_TO_STAGE, headToStage);
            const auto record = recordFromTranslationRotationVector(headToStage);
            mPoseRecorder.record(record);
            mPoseDurableRecorder.record(record);
        }
    }

    if (callback != nullptr) {
        callback->onHeadToSoundStagePoseUpdated(headToStage);
    }
}

void Spatializer::onActualModeChange(HeadTrackingMode mode) {
    std::string modeStr = ToString(mode);
    ALOGV("%s(%s)", __func__, modeStr.c_str());
    sp<AMessage> msg = new AMessage(EngineCallbackHandler::kWhatOnActualModeChange, mHandler);
    msg->setInt32(EngineCallbackHandler::kModeKey, static_cast<int>(mode));
    msg->post();
}

void Spatializer::onActualModeChangeMsg(HeadTrackingMode mode) {
    ALOGV("%s(%s)", __func__, ToString(mode).c_str());
    sp<media::ISpatializerHeadTrackingCallback> callback;
    HeadTracking::Mode spatializerMode;
    {
        audio_utils::lock_guard lock(mMutex);
        if (!mSupportsHeadTracking) {
            spatializerMode = HeadTracking::Mode::DISABLED;
        } else {
            switch (mode) {
                case HeadTrackingMode::STATIC:
                    spatializerMode = HeadTracking::Mode::DISABLED;
                    break;
                case HeadTrackingMode::WORLD_RELATIVE:
                    spatializerMode = HeadTracking::Mode::RELATIVE_WORLD;
                    break;
                case HeadTrackingMode::SCREEN_RELATIVE:
                    spatializerMode = HeadTracking::Mode::RELATIVE_SCREEN;
                    break;
                default:
                    LOG_ALWAYS_FATAL("Unknown mode: %s", ToString(mode).c_str());
            }
        }
        mActualHeadTrackingMode = spatializerMode;
        if (mEngine != nullptr) {
            if (spatializerMode == HeadTracking::Mode::DISABLED) {
                resetEngineHeadPose_l();
            } else {
                setEffectParameter_l(SPATIALIZER_PARAM_HEADTRACKING_MODE,
                                     std::vector<HeadTracking::Mode>{spatializerMode});
                setEngineHeadtrackingConnectionMode_l();
            }
        }
        callback = mHeadTrackingCallback;
        mLocalLog.log("%s: updating mode to %s", __func__, ToString(mode).c_str());
    }
    if (callback != nullptr) {
        callback->onHeadTrackingModeChanged(spatializerMode);
    }
}

void Spatializer::setEngineHeadtrackingConnectionMode_l() {
    if (!com::android::media::audio::dsa_over_bt_le_audio()) {
        return;
    }
    if (mActualHeadTrackingMode != HeadTracking::Mode::DISABLED
            && !mSupportedHeadtrackingConnectionModes.empty()) {
        setEffectParameter_l(SPATIALIZER_PARAM_HEADTRACKING_CONNECTION,
                static_cast<uint8_t>(mHeadtrackingConnectionMode),
                static_cast<uint32_t>(mHeadSensor));
    }
}

void Spatializer::sortSupportedLatencyModes_l() {
    if (!com::android::media::audio::dsa_over_bt_le_audio()) {
        return;
    }
    std::sort(mSupportedLatencyModes.begin(), mSupportedLatencyModes.end(),
            [this](audio_latency_mode_t x, audio_latency_mode_t y) {
                auto itX = std::find(mOrderedLowLatencyModes.begin(),
                    mOrderedLowLatencyModes.end(), x);
                auto itY = std::find(mOrderedLowLatencyModes.begin(),
                    mOrderedLowLatencyModes.end(), y);
                return itX < itY;
            });
}

status_t Spatializer::attachOutput(audio_io_handle_t output,
          const std::vector<audio_channel_mask_t>& activeTracksMasks) {
    bool outputChanged = false;
    sp<media::INativeSpatializerCallback> callback;

    {
        audio_utils::lock_guard lock(mMutex);
        ALOGV("%s output %d mOutput %d", __func__, (int)output, (int)mOutput);
        mLocalLog.log("%s with output %d tracks %zu (mOutput %d)", __func__, (int)output,
                      activeTracksMasks.size(), (int)mOutput);
        if (mOutput != AUDIO_IO_HANDLE_NONE) {
            LOG_ALWAYS_FATAL_IF(mEngine == nullptr, "%s output set without FX engine", __func__);
            // remove FX instance
            mEngine->setEnabled(false);
            mEngine.clear();
            mPoseController.reset();
            AudioSystem::removeSupportedLatencyModesCallback(this);
        }

        // create FX instance on output
        AttributionSourceState attributionSource = AttributionSourceState();
        mEngine = new AudioEffect(attributionSource);
        mEngine->set(nullptr /* type */, &mEngineDescriptor.uuid, 0 /* priority */,
                     wp<AudioEffect::IAudioEffectCallback>::fromExisting(this),
                     AUDIO_SESSION_OUTPUT_STAGE, output, {} /* device */, false /* probe */,
                     true /* notifyFramesProcessed */);
        status_t status = mEngine->initCheck();
        ALOGV("%s mEngine create status %d", __func__, (int)status);
        if (status != NO_ERROR) {
            return status;
        }

        outputChanged = mOutput != output;
        mOutput = output;
        mActiveTracksMasks = activeTracksMasks;
        AudioSystem::addSupportedLatencyModesCallback(this);

        std::vector<audio_latency_mode_t> latencyModes;
        status = AudioSystem::getSupportedLatencyModes(mOutput, &latencyModes);
        if (status == OK) {
            mSupportedLatencyModes = latencyModes;
            sortSupportedLatencyModes_l();
        }

        checkEngineState_l();
        if (mSupportsHeadTracking) {
            checkPoseController_l();
            checkSensorsState_l();
        }
        callback = mSpatializerCallback;

        // Restore common effect state.
        setEffectParameter_l(SPATIALIZER_PARAM_DISPLAY_ORIENTATION,
                std::vector<float>{mDisplayOrientation});
        setEffectParameter_l(SPATIALIZER_PARAM_FOLD_STATE,
                std::vector<uint8_t>{mFoldedState});
        setEffectParameter_l(SPATIALIZER_PARAM_HINGE_ANGLE,
                std::vector<float>{mHingeAngle});
    }

    if (outputChanged && callback != nullptr) {
        callback->onOutputChanged(output);
    }

    return NO_ERROR;
}

audio_io_handle_t Spatializer::detachOutput() {
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    sp<media::INativeSpatializerCallback> callback;

    {
        audio_utils::lock_guard lock(mMutex);
        mLocalLog.log("%s with output %d num tracks %zu",
            __func__, (int)mOutput, mActiveTracksMasks.size());
        ALOGV("%s mOutput %d", __func__, (int)mOutput);
        if (mOutput == AUDIO_IO_HANDLE_NONE) {
            return output;
        }
        // remove FX instance
        mEngine->setEnabled(false);
        mEngine.clear();
        AudioSystem::removeSupportedLatencyModesCallback(this);
        output = mOutput;
        mOutput = AUDIO_IO_HANDLE_NONE;
        mPoseController.reset();
        callback = mSpatializerCallback;
    }

    if (callback != nullptr) {
        callback->onOutputChanged(AUDIO_IO_HANDLE_NONE);
    }
    return output;
}

void Spatializer::onSupportedLatencyModesChanged(
        audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) {
    ALOGV("%s output %d num modes %zu", __func__, (int)output, modes.size());
    sp<AMessage> msg =
            new AMessage(EngineCallbackHandler::kWhatOnLatencyModesChanged, mHandler);
    msg->setObject(EngineCallbackHandler::kLatencyModesKey,
        sp<EngineCallbackHandler::LatencyModes>::make(output, modes));
    msg->post();
}

void Spatializer::onSupportedLatencyModesChangedMsg(
        audio_io_handle_t output, std::vector<audio_latency_mode_t>&& modes) {
    audio_utils::lock_guard lock(mMutex);
    ALOGV("%s output %d mOutput %d num modes %zu",
            __func__, (int)output, (int)mOutput, modes.size());
    if (output == mOutput) {
        mSupportedLatencyModes = std::move(modes);
        sortSupportedLatencyModes_l();
        checkSensorsState_l();
    }
}

void Spatializer::updateActiveTracks(
        const std::vector<audio_channel_mask_t>& activeTracksMasks) {
    audio_utils::lock_guard lock(mMutex);
    if (mActiveTracksMasks != activeTracksMasks) {
        mLocalLog.log("%s from %zu to %zu",
                __func__, mActiveTracksMasks.size(), activeTracksMasks.size());
        mActiveTracksMasks = activeTracksMasks;
        checkEngineState_l();
        checkSensorsState_l();
    }
}

audio_latency_mode_t Spatializer::selectHeadtrackingConnectionMode_l() {
    if (!com::android::media::audio::dsa_over_bt_le_audio()) {
        return AUDIO_LATENCY_MODE_LOW;
    }
    // mSupportedLatencyModes is ordered according to system preferences loaded in
    // mOrderedLowLatencyModes
    mHeadtrackingConnectionMode = HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED;
    audio_latency_mode_t requestedLatencyMode = mSupportedLatencyModes[0];
    if (requestedLatencyMode == AUDIO_LATENCY_MODE_DYNAMIC_SPATIAL_AUDIO_HARDWARE) {
        if (mSupportedHeadtrackingConnectionModes.find(
                HeadTracking::ConnectionMode::DIRECT_TO_SENSOR_TUNNEL)
                    != mSupportedHeadtrackingConnectionModes.end()) {
            mHeadtrackingConnectionMode = HeadTracking::ConnectionMode::DIRECT_TO_SENSOR_TUNNEL;
        } else if (mSupportedHeadtrackingConnectionModes.find(
                HeadTracking::ConnectionMode::DIRECT_TO_SENSOR_SW)
                    != mSupportedHeadtrackingConnectionModes.end()) {
            mHeadtrackingConnectionMode = HeadTracking::ConnectionMode::DIRECT_TO_SENSOR_SW;
        } else {
            // if the engine does not support direct reading of IMU data, do not allow
            // DYNAMIC_SPATIAL_AUDIO_HARDWARE mode and fallback to next mode
            if (mSupportedLatencyModes.size() > 1) {
                requestedLatencyMode = mSupportedLatencyModes[1];
            } else {
                // If only DYNAMIC_SPATIAL_AUDIO_HARDWARE mode is reported by the
                // HAL and the engine does not support it, assert as this is a
                // product configuration error
                LOG_ALWAYS_FATAL("%s: the audio HAL reported only low latency with"
                        "HW HID tunneling but the spatializer does not support it",
                        __func__);
            }
        }
    }
    return requestedLatencyMode;
}

void Spatializer::checkSensorsState_l() {
    mRequestedLatencyMode = AUDIO_LATENCY_MODE_FREE;
    const bool supportsSetLatencyMode = !mSupportedLatencyModes.empty();
    bool supportsLowLatencyMode;
    if (com::android::media::audio::dsa_over_bt_le_audio()) {
        // mSupportedLatencyModes is ordered with MODE_FREE always at the end:
        // the first entry is never MODE_FREE if at least one low ltency mode is supported.
        supportsLowLatencyMode = supportsSetLatencyMode
                && mSupportedLatencyModes[0] != AUDIO_LATENCY_MODE_FREE;
    } else {
        supportsLowLatencyMode = supportsSetLatencyMode && std::find(
            mSupportedLatencyModes.begin(), mSupportedLatencyModes.end(),
            AUDIO_LATENCY_MODE_LOW) != mSupportedLatencyModes.end();
    }
    if (mSupportsHeadTracking) {
        if (mPoseController != nullptr) {
            // TODO(b/253297301, b/255433067) reenable low latency condition check
            // for Head Tracking after Bluetooth HAL supports it correctly.
            if (shouldUseHeadTracking_l() && mLevel != Spatialization::Level::NONE
                    && mDesiredHeadTrackingMode != HeadTrackingMode::STATIC
                    && mHeadSensor != SpatializerPoseController::INVALID_SENSOR) {
                if (supportsLowLatencyMode) {
                    mRequestedLatencyMode = selectHeadtrackingConnectionMode_l();
                }
                if (mEngine != nullptr) {
                    setEffectParameter_l(SPATIALIZER_PARAM_HEADTRACKING_MODE,
                            std::vector<HeadTracking::Mode>{mActualHeadTrackingMode});
                    setEngineHeadtrackingConnectionMode_l();
                }
                // TODO: b/307588546: configure mPoseController according to selected
                // mHeadtrackingConnectionMode
                mPoseController->setHeadSensor(mHeadSensor);
                mPoseController->setScreenSensor(mScreenSensor);
            } else {
                mPoseController->setHeadSensor(SpatializerPoseController::INVALID_SENSOR);
                mPoseController->setScreenSensor(SpatializerPoseController::INVALID_SENSOR);
                resetEngineHeadPose_l();
            }
        } else {
            resetEngineHeadPose_l();
        }
    }
    if (mOutput != AUDIO_IO_HANDLE_NONE && supportsSetLatencyMode) {
        const status_t status =
                AudioSystem::setRequestedLatencyMode(mOutput, mRequestedLatencyMode);
        ALOGD("%s: setRequestedLatencyMode for output thread(%d) to %s returned %d", __func__,
              mOutput, toString(mRequestedLatencyMode).c_str(), status);
    }
}


/* static */
bool Spatializer::containsImmersiveChannelMask(
        const std::vector<audio_channel_mask_t>& masks)
{
    for (auto mask : masks) {
        if (audio_is_channel_mask_spatialized(mask)) {
            return true;
        }
    }
    // Only non-immersive channel masks, e.g. AUDIO_CHANNEL_OUT_STEREO, are present.
    return false;
}

bool Spatializer::shouldUseHeadTracking_l() const {
    // Headtracking only available on immersive channel masks.
    return containsImmersiveChannelMask(mActiveTracksMasks);
}

void Spatializer::checkEngineState_l() {
    if (mEngine != nullptr) {
        if (mLevel != Spatialization::Level::NONE && mActiveTracksMasks.size() > 0) {
            mEngine->setEnabled(true);
            setEffectParameter_l(SPATIALIZER_PARAM_LEVEL,
                    std::vector<Spatialization::Level>{mLevel});
        } else {
            setEffectParameter_l(SPATIALIZER_PARAM_LEVEL,
                    std::vector<Spatialization::Level>{Spatialization::Level::NONE});
            mEngine->setEnabled(false);
        }
    }
}

void Spatializer::checkPoseController_l() {
    bool isControllerNeeded = mDesiredHeadTrackingMode != HeadTrackingMode::STATIC
            && mHeadSensor != SpatializerPoseController::INVALID_SENSOR;

    if (isControllerNeeded && mPoseController == nullptr) {
        mPoseController = std::make_shared<SpatializerPoseController>(
                static_cast<SpatializerPoseController::Listener*>(this),
                10ms, std::nullopt);
        LOG_ALWAYS_FATAL_IF(mPoseController == nullptr,
                            "%s could not allocate pose controller", __func__);
        mPoseController->setDisplayOrientation(mDisplayOrientation);
    } else if (!isControllerNeeded && mPoseController != nullptr) {
        mPoseController.reset();
        resetEngineHeadPose_l();
    }
    if (mPoseController != nullptr) {
        mPoseController->setDesiredMode(mDesiredHeadTrackingMode);
    }
}

void Spatializer::calculateHeadPose() {
    ALOGV("%s", __func__);
    audio_utils::lock_guard lock(mMutex);
    if (mPoseController != nullptr) {
        mPoseController->calculateAsync();
    }
}

void Spatializer::onFramesProcessed(int32_t framesProcessed) {
    sp<AMessage> msg =
            new AMessage(EngineCallbackHandler::kWhatOnFramesProcessed, mHandler);
    msg->setInt32(EngineCallbackHandler::kNumFramesKey, framesProcessed);
    msg->post();
}

std::string Spatializer::toString(unsigned level) const {
    std::string prefixSpace(level, ' ');
    std::string ss = prefixSpace + "Spatializer:\n";
    bool needUnlock = false;

    prefixSpace += ' ';
    if (!mMutex.try_lock()) {
        // dumpsys even try_lock failed, information dump can be useful although may not accurate
        ss.append(prefixSpace).append("try_lock failed, dumpsys below maybe INACCURATE!\n");
    } else {
        needUnlock = true;
    }

    // Spatializer class information.
    // 1. Capabilities (mLevels, mHeadTrackingModes, mSpatializationModes, mChannelMasks, etc)
    ss.append(prefixSpace).append("Supported levels: [");
    for (auto& level : mLevels) {
        base::StringAppendF(&ss, " %s", ToString(level).c_str());
    }
    base::StringAppendF(&ss, "], mLevel: %s", ToString(mLevel).c_str());

    base::StringAppendF(&ss, "\n%smHeadTrackingModes: [", prefixSpace.c_str());
    for (auto& mode : mHeadTrackingModes) {
        base::StringAppendF(&ss, " %s", ToString(mode).c_str());
    }
    base::StringAppendF(&ss, "], Desired: %s, Actual %s\n",
                        ToString(mDesiredHeadTrackingMode).c_str(),
                        ToString(mActualHeadTrackingMode).c_str());

    base::StringAppendF(&ss, "%smSpatializationModes: [", prefixSpace.c_str());
    for (auto& mode : mSpatializationModes) {
        base::StringAppendF(&ss, " %s", ToString(mode).c_str());
    }
    ss += "]\n";

    base::StringAppendF(&ss, "%smChannelMasks: ", prefixSpace.c_str());
    for (auto& mask : mChannelMasks) {
        base::StringAppendF(&ss, "%s", audio_channel_out_mask_to_string(mask));
    }
    base::StringAppendF(&ss, "%smSpatializedChannelMasks: ", prefixSpace.c_str());
    for (auto& mask : mSpatializedChannelMasks) {
        base::StringAppendF(&ss, "%s", audio_channel_out_mask_to_string(mask));
    }
    base::StringAppendF(&ss, "\n%smSupportsHeadTracking: %s\n", prefixSpace.c_str(),
                        mSupportsHeadTracking ? "true" : "false");
    // 2. Settings (Output, tracks)
    base::StringAppendF(&ss, "%sNum Active Tracks: %zu\n",
            prefixSpace.c_str(), mActiveTracksMasks.size());
    base::StringAppendF(&ss, "%sOutputStreamHandle: %d\n", prefixSpace.c_str(), (int)mOutput);

    // 3. Sensors, Effect information.
    base::StringAppendF(&ss, "%sHeadSensorHandle: 0x%08x\n", prefixSpace.c_str(), mHeadSensor);
    base::StringAppendF(&ss, "%sScreenSensorHandle: 0x%08x\n", prefixSpace.c_str(), mScreenSensor);
    base::StringAppendF(&ss, "%sEffectHandle: %p\n", prefixSpace.c_str(), mEngine.get());
    base::StringAppendF(&ss, "%sDisplayOrientation: %f\n", prefixSpace.c_str(),
                        mDisplayOrientation);

    // 4. Show flag or property state.
    base::StringAppendF(
            &ss, "%sStereo Spatialization: %s\n", prefixSpace.c_str(),
            SpatializerHelper::isStereoSpatializationFeatureEnabled() ? "true" : "false");

    ss.append(prefixSpace + "CommandLog:\n");
    ss += mLocalLog.dumpToString((prefixSpace + " ").c_str(), mMaxLocalLogLine);

    // PostController dump.
    if (mPoseController != nullptr) {
        ss.append(mPoseController->toString(level + 1))
            .append(prefixSpace)
            .append("Pose (active stage-to-head) [tx, ty, tz : pitch, roll, yaw]:\n")
            .append(prefixSpace)
            .append(" PerMinuteHistory:\n")
            .append(mPoseDurableRecorder.toString(level + 3))
            .append(prefixSpace)
            .append(" PerSecondHistory:\n")
            .append(mPoseRecorder.toString(level + 3));
    } else {
        ss.append(prefixSpace).append("SpatializerPoseController not exist\n");
    }

    if (needUnlock) {
        mMutex.unlock();
    }
    return ss;
}

} // namespace android
