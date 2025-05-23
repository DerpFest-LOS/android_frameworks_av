/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <policy.h>
#include <system/audio_effect.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <utils/String8.h>

namespace android {

class AudioInputCollection;
class AudioInputDescriptor;
class AudioPolicyClientInterface;

class EffectDescriptor : public RefBase
{
public:
  EffectDescriptor(const effect_descriptor_t* desc, bool isMusicEffect, int id,
                   audio_io_handle_t io, audio_session_t session)
      : mId(id),
        mIo(io),
        mIsOrphan(io == AUDIO_IO_HANDLE_NONE),
        mSession(session),
        mEnabled(false),
        mSuspended(false),
        mIsMusicEffect(isMusicEffect) {
        memcpy (&mDesc, desc, sizeof(effect_descriptor_t));
    }

    void dump(String8 *dst, int spaces = 0) const;

    int mId;                   // effect unique ID
    audio_io_handle_t mIo;     // io the effect is attached to
    bool mIsOrphan = false;    // on creation, effect is not yet attached but not yet orphan
    bool mEnabledWhenMoved = false;    // Backup enabled state before being moved
    audio_session_t mSession;  // audio session the effect is on
    effect_descriptor_t mDesc; // effect descriptor
    bool mEnabled;             // enabled state: CPU load being used or not
    bool mSuspended;           // enabled but suspended by concurent capture policy

    bool isMusicEffect() const { return mIsMusicEffect; }

private:
    bool mIsMusicEffect;
};

class EffectDescriptorCollection : public KeyedVector<int, sp<EffectDescriptor> >
{
public:
    EffectDescriptorCollection();

    status_t registerEffect(const effect_descriptor_t *desc, audio_io_handle_t io,
                            int session, int id, bool isMusicEffect);
    status_t unregisterEffect(int id);
    sp<EffectDescriptor> getEffect(int id) const;
    EffectDescriptorCollection getEffectsForIo(audio_io_handle_t io) const;
    status_t setEffectEnabled(int id, bool enabled);
    bool     isEffectEnabled(int id) const;
    uint32_t getMaxEffectsCpuLoad() const;
    uint32_t getMaxEffectsMemory() const;
    bool isNonOffloadableEffectEnabled(
            const std::optional<const effect_uuid_t>& uuid = std::nullopt) const;

    void moveEffects(audio_session_t session,
                     audio_io_handle_t srcOutput,
                     audio_io_handle_t dstOutput,
                     AudioPolicyClientInterface *clientInterface);
    void moveEffects(const std::vector<int>& ids, audio_io_handle_t dstOutput);
    void moveEffects(audio_session_t sessionId, audio_io_handle_t srcIo, audio_io_handle_t dstIo,
            const AudioInputCollection *inputs, AudioPolicyClientInterface *clientInterface);
    void moveEffectsForIo(audio_session_t sessionId, audio_io_handle_t dstIo,
            const AudioInputCollection *inputs, AudioPolicyClientInterface *mClientInterface);
    void putOrphanEffects(audio_session_t sessionId, audio_io_handle_t srcIo,
            const AudioInputCollection *inputs, AudioPolicyClientInterface *clientInterface);
    void putOrphanEffectsForIo(audio_io_handle_t srcIo);

    /**
     * @brief Checks if an effect session was already attached to an io handle and return it if
     * found. Check only for a given effect type if effectType is not null or for any effect
     * otherwise.
     * @param sessionId to consider.
     * @param effectType to consider.
     * @return ioHandle if found, AUDIO_IO_HANDLE_NONE otherwise.
     */
    audio_io_handle_t getIoForSession(audio_session_t sessionId,
                                      const effect_uuid_t* effectType = nullptr) const;

    /**
     * @brief Checks if there is at least one orphan effect with given sessionId and optional effect
     * type uuid.
     * @param sessionId Session ID.
     * @param effectType Optional effect type UUID pointer to effect_uuid_t, nullptr by default.
     * @return True if there is an orphan effect for given sessionId and type UUID, false otherwise.
     */
    bool hasOrphansForSession(audio_session_t sessionId,
                              const effect_uuid_t* effectType = nullptr) const;

    EffectDescriptorCollection getOrphanEffectsForSession(audio_session_t sessionId) const;
    void dump(String8 *dst, int spaces = 0, bool verbose = true) const;

private:
    status_t setEffectEnabled(const sp<EffectDescriptor> &effectDesc, bool enabled);

    uint32_t mTotalEffectsCpuLoad; // current CPU load used by effects (in MIPS)
    uint32_t mTotalEffectsMemory;  // current memory used by effects (in KB)
    uint32_t mTotalEffectsMemoryMaxUsed; // maximum memory used by effects (in KB)

    /**
     * Maximum CPU load allocated to audio effects in 0.1 MIPS (ARMv5TE, 0 WS memory) units
     */
    static const uint32_t MAX_EFFECTS_CPU_LOAD = 1000;
    /**
     * Maximum memory allocated to audio effects in KB
     */
    static const uint32_t MAX_EFFECTS_MEMORY = 512;
};

} // namespace android
