/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef ANDROID_MEDIA_SPATIALIZER_H
#define ANDROID_MEDIA_SPATIALIZER_H

#include <android-base/stringprintf.h>
#include <android/media/BnEffect.h>
#include <android/media/BnSpatializer.h>
#include <android/media/audio/common/AudioLatencyMode.h>
#include <android/media/audio/common/HeadTracking.h>
#include <android/media/audio/common/Spatialization.h>
#include <audio_utils/mutex.h>
#include <audio_utils/SimpleLog.h>
#include <math.h>
#include <media/AudioEffect.h>
#include <media/MediaMetricsItem.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/VectorRecorder.h>
#include <media/audiohal/EffectHalInterface.h>
#include <media/stagefright/foundation/ALooper.h>
#include <system/audio_effects/effect_spatializer.h>
#include <string>
#include <unordered_set>

#include "SpatializerPoseController.h"

namespace android {


// ----------------------------------------------------------------------------

/**
 * A callback interface from the Spatializer object or its parent AudioPolicyService.
 * This is implemented by the audio policy service hosting the Spatializer to perform
 * actions needed when a state change inside the Spatializer requires some audio system
 * changes that cannot be performed by the Spatializer. For instance opening or closing a
 * spatializer output stream when the spatializer is enabled or disabled
 */
class SpatializerPolicyCallback {
public:
    /** Called when a stage change occurs that requires the parent audio policy service to take
     * some action.
     */
    virtual void onCheckSpatializer() = 0;

    virtual ~SpatializerPolicyCallback() = default;
};
/**
 * The Spatializer class implements all functional controlling the multichannel spatializer
 * with head tracking implementation in the native audio service: audio policy and audio flinger.
 * It presents an AIDL interface available to the java audio service to discover the availability
 * of the feature and options, control its state and register an active head tracking sensor.
 * It maintains the current state of the platform spatializer and applies the stored parameters
 * when the spatializer engine is created and enabled.
 * Based on the requested spatializer level, it will request the creation of a specialized output
 * mixer to the audio policy service which will in turn notify the Spatializer of the output
 * stream on which a spatializer engine should be created, configured and enabled.
 * The spatializer also hosts the head tracking management logic. This logic receives the
 * desired head tracking mode and selected head tracking sensor, registers a sensor event listener
 * and derives the compounded head pose information to the spatializer engine.
 *
 * Workflow:
 * - Initialization: when the audio policy service starts, it checks if a spatializer effect
 * engine exists and if the audio policy manager reports a dedicated spatializer output profile.
 * If both conditions are met, a Spatializer object is created
 * - Capabilities discovery: AudioService will call AudioSystem::canBeSpatialized() and if true,
 * acquire an ISpatializer interface with AudioSystem::getSpatializer(). This interface
 * will be used to query the implementation capabilities and configure the spatializer.
 * - Enabling: when ISpatializer::setLevel() sets a level different from NONE the spatializer
 * is considered enabled. The audio policy callback onCheckSpatializer() is called. This
 * triggers a request to audio policy manager to open a spatialization output stream and a
 * spatializer mixer is created in audio flinger. When an output is returned by audio policy
 * manager, Spatializer::attachOutput() is called which creates and enables the spatializer
 * stage engine on the specified output.
 * - Disabling: when the spatialization level is set to NONE, the spatializer is considered
 * disabled. The audio policy callback onCheckSpatializer() is called. This triggers a call
 * to Spatializer::detachOutput() and the spatializer engine is released. Then a request is
 * made to audio policy manager to release and close the spatializer output stream and the
 * spatializer mixer thread is destroyed.
 */
class Spatializer : public media::BnSpatializer,
                    public AudioEffect::IAudioEffectCallback,
                    public IBinder::DeathRecipient,
                    private SpatializerPoseController::Listener,
                    public virtual AudioSystem::SupportedLatencyModesCallback {
  public:
    static sp<Spatializer> create(SpatializerPolicyCallback* callback,
                                  const sp<EffectsFactoryHalInterface>& effectsFactoryHal);

           ~Spatializer() override;

    /** RefBase */
    void onFirstRef();

    /** ISpatializer, see ISpatializer.aidl */
    binder::Status release() override;
    binder::Status getSupportedLevels(
            std::vector<media::audio::common::Spatialization::Level>* levels) override;
    binder::Status setLevel(media::audio::common::Spatialization::Level level) override;
    binder::Status getLevel(media::audio::common::Spatialization::Level *level) override;
    binder::Status isHeadTrackingSupported(bool *supports);
    binder::Status getSupportedHeadTrackingModes(
            std::vector<media::audio::common::HeadTracking::Mode>* modes) override;
    binder::Status setDesiredHeadTrackingMode(
            media::audio::common::HeadTracking::Mode mode) override;
    binder::Status getActualHeadTrackingMode(
            media::audio::common::HeadTracking::Mode* mode) override;
    binder::Status recenterHeadTracker() override;
    binder::Status setGlobalTransform(const std::vector<float>& screenToStage) override;
    binder::Status setHeadSensor(int sensorHandle) override;
    binder::Status setScreenSensor(int sensorHandle) override;
    binder::Status setDisplayOrientation(float physicalToLogicalAngle) override;
    binder::Status setHingeAngle(float hingeAngle) override;
    binder::Status setFoldState(bool folded) override;
    binder::Status getSupportedModes(
            std::vector<media::audio::common::Spatialization::Mode>* modes) override;
    binder::Status registerHeadTrackingCallback(
        const sp<media::ISpatializerHeadTrackingCallback>& callback) override;
    binder::Status setParameter(int key, const std::vector<unsigned char>& value) override;
    binder::Status getParameter(int key, std::vector<unsigned char> *value) override;
    binder::Status getOutput(int *output);
    binder::Status getSpatializedChannelMasks(std::vector<int>* masks) override;

    /** IBinder::DeathRecipient. Listen to the death of the INativeSpatializerCallback. */
    virtual void binderDied(const wp<IBinder>& who);

    /** SupportedLatencyModesCallback */
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) override;

    /** Registers a INativeSpatializerCallback when a client is attached to this Spatializer
     * by audio policy service.
     */
    status_t registerCallback(const sp<media::INativeSpatializerCallback>& callback);

    status_t loadEngineConfiguration(sp<EffectHalInterface> effect);

    /** Level getter for use by local classes. */
    media::audio::common::Spatialization::Level getLevel() const {
        audio_utils::lock_guard lock(mMutex);
        return mLevel;
    }

    /** For test only */
    std::unordered_set<media::audio::common::HeadTracking::ConnectionMode>
            getSupportedHeadtrackingConnectionModes() const {
        return mSupportedHeadtrackingConnectionModes;
    }

    /** For test only */
    media::audio::common::HeadTracking::ConnectionMode getHeadtrackingConnectionMode() const {
        return mHeadtrackingConnectionMode;
    }

    /** For test only */
    std::vector<audio_latency_mode_t> getSupportedLatencyModes() const {
        audio_utils::lock_guard lock(mMutex);
        return mSupportedLatencyModes;
    }

    /** For test only */
    std::vector<audio_latency_mode_t> getOrderedLowLatencyModes() const {
        return mOrderedLowLatencyModes;
    }

    /** For test only */
    audio_latency_mode_t getRequestedLatencyMode() const {
        audio_utils::lock_guard lock(mMutex);
        return mRequestedLatencyMode;
    }

    /** Called by audio policy service when the special output mixer dedicated to spatialization
     * is opened and the spatializer engine must be created.
     */
    status_t attachOutput(audio_io_handle_t output,
                          const std::vector<audio_channel_mask_t>& activeTracksMasks);
    /** Called by audio policy service when the special output mixer dedicated to spatialization
     * is closed and the spatializer engine must be release.
     */
    audio_io_handle_t detachOutput();
    /** Returns the output stream the spatializer is attached to. */
    audio_io_handle_t getOutput() const { audio_utils::lock_guard lock(mMutex); return mOutput; }

    /** For test only */
    void setOutput(audio_io_handle_t output) {
        audio_utils::lock_guard lock(mMutex);
        mOutput = output;
    }

    void updateActiveTracks(const std::vector<audio_channel_mask_t>& activeTracksMasks);

    /** Gets the channel mask, sampling rate and format set for the spatializer input. */
    audio_config_base_t getAudioInConfig() const;

    void calculateHeadPose();

    /** Convert fields in Spatializer and sub-modules to a string. Disable thread-safety-analysis
     * here because we want to dump mutex guarded members even try_lock failed to provide as much
     * information as possible for debugging purpose. */
    std::string toString(unsigned level) const NO_THREAD_SAFETY_ANALYSIS;

    static std::string toString(audio_latency_mode_t mode) {
        // We convert to the AIDL type to print (eventually the legacy type will be removed).
        const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
        return result.has_value() ?
                media::audio::common::toString(*result) : "unknown_latency_mode";
    }

    // If the Spatializer is not created, we send the status for metrics purposes.
    // OK:      Spatializer not expected to be created.
    // NO_INIT: Spatializer creation failed.
    static void sendEmptyCreateSpatializerMetricWithStatus(status_t status);

    /** Made public for test only */
    void onSupportedLatencyModesChangedMsg(
            audio_io_handle_t output, std::vector<audio_latency_mode_t>&& modes);

    // Made public for test only
    /**
     * Returns true if there exists an immersive channel mask in the vector.
     *
     * Example of a non-immersive channel mask such as AUDIO_CHANNEL_OUT_STEREO
     * versus an immersive channel mask such as AUDIO_CHANNEL_OUT_5POINT1.
     */
    static bool containsImmersiveChannelMask(
            const std::vector<audio_channel_mask_t>& masks);

private:
    Spatializer(effect_descriptor_t engineDescriptor,
                     SpatializerPolicyCallback *callback);

    static void engineCallback(int32_t event, void* user, void *info);

    // From VirtualizerStageController::Listener
    void onHeadToStagePose(const media::Pose3f& headToStage) override;
    void onActualModeChange(media::HeadTrackingMode mode) override;

    void onHeadToStagePoseMsg(const std::vector<float>& headToStage);
    void onActualModeChangeMsg(media::HeadTrackingMode mode);

    static constexpr int kMaxEffectParamValues = 10;
    /**
     * Get a parameter from spatializer engine by calling the effect HAL command method directly.
     * To be used when the engine instance mEngine is not yet created in the effect framework.
     * When MULTI_VALUES is false, the expected reply is only one value of type T.
     * When MULTI_VALUES is true, the expected reply is made of a number (of type T) indicating
     * how many values are returned, followed by this number for values of type T.
     */
    template<bool MULTI_VALUES, typename T>
    status_t getHalParameter(sp<EffectHalInterface> effect, uint32_t type,
                                          std::vector<T> *values) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");

        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1];
        uint32_t reply[sizeof(effect_param_t) / sizeof(uint32_t) + 2 + kMaxEffectParamValues];

        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        if (MULTI_VALUES) {
            p->vsize = (kMaxEffectParamValues + 1) * sizeof(T);
        } else {
            p->vsize = sizeof(T);
        }
        *(uint32_t *)p->data = type;
        uint32_t replySize = sizeof(effect_param_t) + p->psize + p->vsize;

        status_t status = effect->command(EFFECT_CMD_GET_PARAM,
                                          sizeof(effect_param_t) + sizeof(uint32_t), cmd,
                                          &replySize, reply);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        if (replySize <
                sizeof(effect_param_t) + sizeof(uint32_t) + (MULTI_VALUES ? 2 : 1) * sizeof(T)) {
            return BAD_VALUE;
        }

        T *params = (T *)((uint8_t *)reply + sizeof(effect_param_t) + sizeof(uint32_t));
        int numParams = 1;
        if (MULTI_VALUES) {
            numParams = (int)*params++;
        }
        if (numParams > kMaxEffectParamValues) {
            return BAD_VALUE;
        }
        (*values).clear();
        std::copy(&params[0], &params[numParams], back_inserter(*values));
        return NO_ERROR;
    }

    /**
     * Set a parameter to spatializer engine by calling setParameter on mEngine AudioEffect object.
     * It is possible to pass more than one value of type T according to the parameter type
     *  according to values vector size.
     */
    template<typename T>
    status_t setEffectParameter_l(uint32_t type, const std::vector<T>& values) REQUIRES(mMutex) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");

        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values.size()];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values.size();
        *(uint32_t *)p->data = type;
        memcpy((uint32_t *)p->data + 1, values.data(), sizeof(T) * values.size());

        status_t status = mEngine->setParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        return NO_ERROR;
    }

    /**
     * Set a parameter to spatializer engine by calling setParameter on mEngine AudioEffect object.
     * The variant is for compound parameters with two values of different base types
     */
    template<typename P1, typename P2>
    status_t setEffectParameter_l(uint32_t type, const P1 val1, const P2 val2) REQUIRES(mMutex) {
        static_assert(sizeof(P1) <= sizeof(uint32_t), "The size of P1 must less than 32 bits");
        static_assert(sizeof(P2) <= sizeof(uint32_t), "The size of P2 must less than 32 bits");

        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 3];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = 2 * sizeof(uint32_t);
        *(uint32_t *)p->data = type;
        *((uint32_t *)p->data + 1) = static_cast<uint32_t>(val1);
        *((uint32_t *)p->data + 2) = static_cast<uint32_t>(val2);

        status_t status = mEngine->setParameter(p);
        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        return NO_ERROR;
    }

    /**
     * Get a parameter from spatializer engine by calling getParameter on AudioEffect object.
     * It is possible to read more than one value of type T according to the parameter type
     * by specifying values vector size.
     */
    template<typename T>
    status_t getEffectParameter_l(uint32_t type, std::vector<T> *values) REQUIRES(mMutex) {
        static_assert(sizeof(T) <= sizeof(uint32_t), "The size of T must less than 32 bits");

        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 1 + values->size()];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(T) * values->size();
        *(uint32_t *)p->data = type;

        status_t status = mEngine->getParameter(p);

        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }

        int numValues = std::min(p->vsize / sizeof(T), values->size());
        (*values).clear();
        T *retValues = (T *)((uint8_t *)p->data + sizeof(uint32_t));
        std::copy(&retValues[0], &retValues[numValues], back_inserter(*values));

        return NO_ERROR;
    }

    /**
     * Get a parameter from spatializer engine by calling getParameter on AudioEffect object.
     * The variant is for compound parameters with two values of different base types
     */
    template<typename P1, typename P2>
    status_t getEffectParameter_l(uint32_t type, P1 *val1, P2 *val2) REQUIRES(mMutex) {
        static_assert(sizeof(P1) <= sizeof(uint32_t), "The size of P1 must less than 32 bits");
        static_assert(sizeof(P2) <= sizeof(uint32_t), "The size of P2 must less than 32 bits");

        uint32_t cmd[sizeof(effect_param_t) / sizeof(uint32_t) + 3];
        effect_param_t *p = (effect_param_t *)cmd;
        p->psize = sizeof(uint32_t);
        p->vsize = 2 * sizeof(uint32_t);
        *(uint32_t *)p->data = type;

        status_t status = mEngine->getParameter(p);

        if (status != NO_ERROR) {
            return status;
        }
        if (p->status != NO_ERROR) {
            return p->status;
        }
        *val1 = static_cast<P1>(*((uint32_t *)p->data + 1));
        *val2 = static_cast<P2>(*((uint32_t *)p->data + 2));
        return NO_ERROR;
    }

    virtual void onFramesProcessed(int32_t framesProcessed) override;

    /**
     * Checks if head and screen sensors must be actively monitored based on
     * spatializer state and playback activity and configures the pose controller
     * accordingly.
     */
    void checkSensorsState_l() REQUIRES(mMutex);

    /**
     * Checks if the head pose controller should be created or destroyed according
     * to desired head tracking mode.
     */
    void checkPoseController_l() REQUIRES(mMutex);

    /**
     * Checks if the spatializer effect should be enabled based on
     * playback activity and requested level.
     */
    void checkEngineState_l() REQUIRES(mMutex);

    /**
     * Reset head tracking mode and recenter pose in engine: Called when the head tracking
     * is disabled.
     */
    void resetEngineHeadPose_l() REQUIRES(mMutex);

    /** Read bluetooth.core.le.dsa_transport_preference property and populate the ordered list of
     * preferred low latency modes in mOrderedLowLatencyModes.
     */
    void loadOrderedLowLatencyModes();

    /**
     * Sort mSupportedLatencyModes list according to the preference order stored in
     * mOrderedLowLatencyModes.
     * Note: Because MODE_FREE is not in mOrderedLowLatencyModes, it will always be at
     * the end of the list.
     */
    void sortSupportedLatencyModes_l() REQUIRES(mMutex);

    /**
     * Called after enabling head tracking in the spatializer engine to indicate which
     * connection mode should be used among those supported. The selection depends on
     * currently supported latency modes reported by the audio HAL.
     * When the connection mode is direct to the sensor, the sensor ID is also communicated
     * to the spatializer engine.
     */
    void setEngineHeadtrackingConnectionMode_l() REQUIRES(mMutex);

    /**
     * Select the desired head tracking connection mode for the spatializer engine among the list
     * stored in mSupportedHeadtrackingConnectionModes at init time.
     * Also returns the desired low latency mode according to selected connection mode.
     */
    audio_latency_mode_t selectHeadtrackingConnectionMode_l() REQUIRES(mMutex);

    /**
     * Indicates if current conditions are compatible with head tracking.
     */
    bool shouldUseHeadTracking_l() const REQUIRES(mMutex);

    /** Effect engine descriptor */
    const effect_descriptor_t mEngineDescriptor;
    /** Callback interface to parent audio policy service */
    SpatializerPolicyCallback* const mPolicyCallback;

    /** Currently there is only one version of the spatializer running */
    static constexpr const char* kDefaultMetricsId =
            AMEDIAMETRICS_KEY_PREFIX_AUDIO_SPATIALIZER "0";
    const std::string mMetricsId = kDefaultMetricsId;

    /** Mutex protecting internal state */
    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kSpatializer_Mutex};

    /** Client AudioEffect for the engine */
    sp<AudioEffect> mEngine GUARDED_BY(mMutex);
    /** Output stream the spatializer mixer thread is attached to */
    audio_io_handle_t mOutput GUARDED_BY(mMutex) = AUDIO_IO_HANDLE_NONE;

    /** Callback interface to the client (AudioService) controlling this`Spatializer */
    sp<media::INativeSpatializerCallback> mSpatializerCallback GUARDED_BY(mMutex);

    /** Callback interface for head tracking */
    sp<media::ISpatializerHeadTrackingCallback> mHeadTrackingCallback GUARDED_BY(mMutex);

    /** Requested spatialization level */
    media::audio::common::Spatialization::Level mLevel GUARDED_BY(mMutex) =
            media::audio::common::Spatialization::Level::NONE;

    /** Control logic for head-tracking, etc. */
    std::shared_ptr<SpatializerPoseController> mPoseController GUARDED_BY(mMutex);

    /** Last requested head tracking mode */
    media::HeadTrackingMode mDesiredHeadTrackingMode GUARDED_BY(mMutex)
            = media::HeadTrackingMode::STATIC;

    /** Last-reported actual head-tracking mode. */
    media::audio::common::HeadTracking::Mode mActualHeadTrackingMode GUARDED_BY(mMutex)
            = media::audio::common::HeadTracking::Mode::DISABLED;

    /** Selected Head pose sensor */
    int32_t mHeadSensor GUARDED_BY(mMutex) = SpatializerPoseController::INVALID_SENSOR;

    /** Selected Screen pose sensor */
    int32_t mScreenSensor GUARDED_BY(mMutex) = SpatializerPoseController::INVALID_SENSOR;

    /** Last display orientation received */
    float mDisplayOrientation GUARDED_BY(mMutex) = 0.f;  // aligned to natural up orientation.

    /** Last folded state */
    bool mFoldedState GUARDED_BY(mMutex) = false;  // foldable: true means folded.

    /** Last hinge angle */
    float mHingeAngle GUARDED_BY(mMutex) = 0.f;  // foldable: 0.f is closed, M_PI flat open.

    std::vector<media::audio::common::Spatialization::Level> mLevels;
    std::vector<media::audio::common::HeadTracking::Mode> mHeadTrackingModes;
    std::vector<media::audio::common::Spatialization::Mode> mSpatializationModes;
    std::vector<audio_channel_mask_t> mChannelMasks;
    std::vector<audio_channel_mask_t> mSpatializedChannelMasks;
    bool mSupportsHeadTracking;

    /** List of supported head tracking connection modes reported by the spatializer.
     * If the list is empty, the spatializer does not support any optional connection
     * mode and mode HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED is assumed.
     * This is set in the factory constructor and can be accessed without mutex.
     */
    std::unordered_set<media::audio::common::HeadTracking::ConnectionMode>
            mSupportedHeadtrackingConnectionModes;
    /** Selected HT connection mode when several modes are supported by the spatializer */
    media::audio::common::HeadTracking::ConnectionMode mHeadtrackingConnectionMode =
            media::audio::common::HeadTracking::ConnectionMode::FRAMEWORK_PROCESSED;

    // Looper thread for mEngine callbacks
    class EngineCallbackHandler;

    sp<ALooper> mLooper;
    sp<EngineCallbackHandler> mHandler;

    std::vector<audio_channel_mask_t> mActiveTracksMasks GUARDED_BY(mMutex);
    std::vector<audio_latency_mode_t> mSupportedLatencyModes GUARDED_BY(mMutex);
    /** preference order for low latency modes according to persist.bluetooth.hid.transport */
    std::vector<audio_latency_mode_t> mOrderedLowLatencyModes;

    audio_latency_mode_t mRequestedLatencyMode GUARDED_BY(mMutex) = AUDIO_LATENCY_MODE_FREE;

    /** string to latency mode map used to parse bluetooth.core.le.dsa_transport_preference */
    static const std::map<std::string, audio_latency_mode_t> sStringToLatencyModeMap;
    static const std::vector<const char*> sHeadPoseKeys;

    // Local log for command messages.
    static constexpr int mMaxLocalLogLine = 10;
    SimpleLog mLocalLog{mMaxLocalLogLine};

    /**
     * @brief Calculate and record sensor data.
     * Dump to local log with max/average pose angle every mPoseRecordThreshold.
     */
    // Record one log line per second (up to mMaxLocalLogLine) to capture most recent sensor data.
    media::VectorRecorder mPoseRecorder GUARDED_BY(mMutex) {
        6 /* vectorSize */, std::chrono::seconds(1), mMaxLocalLogLine, { 3 } /* delimiterIdx */};
    // Record one log line per minute (up to mMaxLocalLogLine) to capture durable sensor data.
    media::VectorRecorder mPoseDurableRecorder GUARDED_BY(mMutex) {
        6 /* vectorSize */, std::chrono::minutes(1), mMaxLocalLogLine, { 3 } /* delimiterIdx */};
};  // Spatializer

}; // namespace android

#endif // ANDROID_MEDIA_SPATIALIZER_H
