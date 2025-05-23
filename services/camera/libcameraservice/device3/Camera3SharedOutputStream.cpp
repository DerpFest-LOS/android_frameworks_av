/*
 * Copyright (C) 2016-2018 The Android Open Source Project
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

#define LOG_TAG "Camera3-SharedOuStrm"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Trace.h>

#include "Flags.h"

#include "Camera3SharedOutputStream.h"

namespace android {

namespace camera3 {

const size_t Camera3SharedOutputStream::kMaxOutputs;

Camera3SharedOutputStream::Camera3SharedOutputStream(int id,
        const std::vector<SurfaceHolder>& surfaces,
        uint32_t width, uint32_t height, int format,
        uint64_t consumerUsage, android_dataspace dataSpace,
        camera_stream_rotation_t rotation,
        nsecs_t timestampOffset, const std::string& physicalCameraId,
        const std::unordered_set<int32_t> &sensorPixelModesUsed, IPCTransport transport,
        int setId, bool useHalBufManager, int64_t dynamicProfile,
        int64_t streamUseCase, bool deviceTimeBaseIsRealtime, int timestampBase,
        int32_t colorSpace, bool useReadoutTimestamp) :
        Camera3OutputStream(id, CAMERA_STREAM_OUTPUT, width, height,
                            format, dataSpace, rotation, physicalCameraId, sensorPixelModesUsed,
                            transport, consumerUsage, timestampOffset, setId,
                            /*isMultiResolution*/false, dynamicProfile, streamUseCase,
                            deviceTimeBaseIsRealtime, timestampBase, colorSpace,
                            useReadoutTimestamp),
        mUseHalBufManager(useHalBufManager) {
    size_t consumerCount = std::min(surfaces.size(), kMaxOutputs);
    if (surfaces.size() > consumerCount) {
        ALOGE("%s: Trying to add more consumers than the maximum ", __func__);
    }
    for (size_t i = 0; i < consumerCount; i++) {
        mSurfaceUniqueIds[i] = SurfaceHolderUniqueId{surfaces[i], mNextUniqueSurfaceId++};
    }
}

Camera3SharedOutputStream::~Camera3SharedOutputStream() {
    disconnectLocked();
}

status_t Camera3SharedOutputStream::connectStreamSplitterLocked() {
    status_t res = OK;

#if USE_NEW_STREAM_SPLITTER
    mStreamSplitter = sp<Camera3StreamSplitter>::make(mUseHalBufManager);
#else
    mStreamSplitter = sp<DeprecatedCamera3StreamSplitter>::make(mUseHalBufManager);
#endif  // USE_NEW_STREAM_SPLITTER

    uint64_t usage = 0;
    getEndpointUsage(&usage);

    std::unordered_map<size_t, sp<Surface>> initialSurfaces;
    for (size_t i = 0; i < kMaxOutputs; i++) {
        if (mSurfaceUniqueIds[i].mSurfaceHolder.mSurface != nullptr) {
            initialSurfaces.emplace(i, mSurfaceUniqueIds[i].mSurfaceHolder.mSurface);
        }
    }

    res = mStreamSplitter->connect(initialSurfaces, usage, mUsage, camera_stream::max_buffers,
            getWidth(), getHeight(), getFormat(), &mConsumer, camera_stream::dynamic_range_profile);
    if (res != OK) {
        ALOGE("%s: Failed to connect to stream splitter: %s(%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    return res;
}

status_t Camera3SharedOutputStream::attachBufferToSplitterLocked(
        ANativeWindowBuffer* anb,
        const std::vector<size_t>& surface_ids) {
    status_t res = OK;

    // Attach the buffer to the splitter output queues. This could block if
    // the output queue doesn't have any empty slot. So unlock during the course
    // of attachBufferToOutputs.
#if USE_NEW_STREAM_SPLITTER
    sp<Camera3StreamSplitter> splitter = mStreamSplitter;
#else
    sp<DeprecatedCamera3StreamSplitter> splitter = mStreamSplitter;
#endif  // USE_NEW_STREAM_SPLITTER
    mLock.unlock();
    res = splitter->attachBufferToOutputs(anb, surface_ids);
    mLock.lock();
    if (res != OK) {
        ALOGE("%s: Stream %d: Cannot attach stream splitter buffer to outputs: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        // Only transition to STATE_ABANDONED from STATE_CONFIGURED. (If it is STATE_PREPARING,
        // let prepareNextBuffer handle the error.)
        if (res == NO_INIT && mState == STATE_CONFIGURED) {
            mState = STATE_ABANDONED;
        }
    }
    return res;
}

void Camera3SharedOutputStream::setHalBufferManager(bool enabled) {
    Mutex::Autolock l(mLock);
    mUseHalBufManager = enabled;
    if (mStreamSplitter != nullptr) {
        mStreamSplitter->setHalBufferManager(enabled);
    }
}

status_t Camera3SharedOutputStream::notifyBufferReleased(ANativeWindowBuffer *anwBuffer) {
    Mutex::Autolock l(mLock);
    status_t res = OK;
    const sp<GraphicBuffer> buffer(static_cast<GraphicBuffer*>(anwBuffer));

    if (mStreamSplitter != nullptr) {
        res = mStreamSplitter->notifyBufferReleased(buffer);
    }

    return res;
}

bool Camera3SharedOutputStream::isConsumerConfigurationDeferred(size_t surface_id) const {
    Mutex::Autolock l(mLock);
    if (surface_id >= kMaxOutputs) {
        return true;
    }

    return (mSurfaceUniqueIds[surface_id].mSurfaceHolder.mSurface == nullptr);
}

status_t Camera3SharedOutputStream::setConsumers(const std::vector<SurfaceHolder>& surfaceHolders) {
    Mutex::Autolock l(mLock);
    if (surfaceHolders.size() == 0) {
        ALOGE("%s: it's illegal to set zero consumer surfaces!", __FUNCTION__);
        return INVALID_OPERATION;
    }

    status_t ret = OK;
    for (auto& surfaceHolder : surfaceHolders) {
        if (surfaceHolder.mSurface == nullptr) {
            ALOGE("%s: it's illegal to set a null consumer surface!", __FUNCTION__);
            return INVALID_OPERATION;
        }

        ssize_t id = getNextSurfaceIdLocked();
        if (id < 0) {
            ALOGE("%s: No surface ids available!", __func__);
            return NO_MEMORY;
        }

        mSurfaceUniqueIds[id] = SurfaceHolderUniqueId{surfaceHolder, mNextUniqueSurfaceId++};

        // Only call addOutput if the splitter has been connected.
        if (mStreamSplitter != nullptr) {
            ret = mStreamSplitter->addOutput(id, surfaceHolder.mSurface);
            if (ret != OK) {
                ALOGE("%s: addOutput failed with error code %d", __FUNCTION__, ret);
                return ret;

            }
        }
    }
    return ret;
}

status_t Camera3SharedOutputStream::getBufferLocked(camera_stream_buffer *buffer,
        const std::vector<size_t>& surfaceIds) {
    ANativeWindowBuffer* anb;
    int fenceFd = -1;

    status_t res;
    res = getBufferLockedCommon(&anb, &fenceFd);
    if (res != OK) {
        return res;
    }

    if (!mUseHalBufManager) {
        res = attachBufferToSplitterLocked(anb, surfaceIds);
        if (res != OK) {
            return res;
        }
    }

    /**
     * FenceFD now owned by HAL except in case of error,
     * in which case we reassign it to acquire_fence
     */
    handoutBufferLocked(*buffer, &(anb->handle), /*acquireFence*/fenceFd,
                        /*releaseFence*/-1, CAMERA_BUFFER_STATUS_OK, /*output*/true);

    return OK;
}

status_t Camera3SharedOutputStream::queueBufferToConsumer(sp<ANativeWindow>& consumer,
            ANativeWindowBuffer* buffer, int anwReleaseFence,
            const std::vector<size_t>& uniqueSurfaceIds) {
    status_t res = OK;
    if (mUseHalBufManager) {
        if (uniqueSurfaceIds.size() == 0) {
            ALOGE("%s: uniqueSurfaceIds must not be empty!", __FUNCTION__);
            return BAD_VALUE;
        }
        Mutex::Autolock l(mLock);
        std::vector<size_t> surfaceIds;
        for (const auto& uniqueId : uniqueSurfaceIds) {
            bool uniqueIdFound = false;
            for (size_t i = 0; i < kMaxOutputs; i++) {
                if (mSurfaceUniqueIds[i].mId == uniqueId) {
                    surfaceIds.push_back(i);
                    uniqueIdFound = true;
                    break;
                }
            }
            if (!uniqueIdFound) {
                ALOGV("%s: unknown unique surface ID %zu for stream %d: "
                        "output might have been removed.",
                        __FUNCTION__, uniqueId, mId);
            }
        }
        res = attachBufferToSplitterLocked(buffer, surfaceIds);
        if (res != OK) {
            return res;
        }
    }

    res = consumer->queueBuffer(consumer.get(), buffer, anwReleaseFence);

    // After queuing buffer to the internal consumer queue, check whether the buffer is
    // successfully queued to the output queues.
    if (res == OK) {
        res = mStreamSplitter->getOnFrameAvailableResult();
        if (res != OK) {
            ALOGE("%s: getOnFrameAvailable returns %d", __FUNCTION__, res);
        }
    } else {
        ALOGE("%s: queueBufer failed %d", __FUNCTION__, res);
    }

    return res;
}

status_t Camera3SharedOutputStream::configureQueueLocked() {
    status_t res;

    if ((res = Camera3IOStreamBase::configureQueueLocked()) != OK) {
        return res;
    }

    res = connectStreamSplitterLocked();
    if (res != OK) {
        ALOGE("Cannot connect to stream splitter: %s(%d)", strerror(-res), res);
        return res;
    }

    res = configureConsumerQueueLocked(false/*allowPreviewRespace*/);
    if (res != OK) {
        ALOGE("Failed to configureConsumerQueueLocked: %s(%d)", strerror(-res), res);
        return res;
    }

    // Set buffer transform for all configured surfaces
    for (const auto& surfaceUniqueId : mSurfaceUniqueIds) {
        const sp<Surface>& surface = surfaceUniqueId.mSurfaceHolder.mSurface;
        int surfaceId = surfaceUniqueId.mId;
        int32_t transform = surfaceUniqueId.mTransform;
        if (transform == -1 || surface == nullptr) {
            continue;
        }

        res = mStreamSplitter->setTransform(surfaceId, transform);
        if (res != OK) {
            ALOGE("%s: StreamSplitter failed to setTransform: %s(%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }
    }

    return OK;
}

status_t Camera3SharedOutputStream::disconnectLocked() {
    status_t res;
    res = Camera3OutputStream::disconnectLocked();

    if (mStreamSplitter != nullptr) {
        mStreamSplitter->disconnect();
    }

    return res;
}

status_t Camera3SharedOutputStream::getEndpointUsage(uint64_t *usage) {

    status_t res = OK;
    uint64_t u = 0;

    if (mConsumer == nullptr) {
        // Called before shared buffer queue is constructed.
        *usage = getPresetConsumerUsage();

        for (size_t id = 0; id < kMaxOutputs; id++) {
            const auto& surface = mSurfaceUniqueIds[id].mSurfaceHolder.mSurface;
            if (surface != nullptr) {
                res = getEndpointUsageForSurface(&u, surface);
                *usage |= u;
            }
        }
    } else {
        // Called after shared buffer queue is constructed.
        res = getEndpointUsageForSurface(&u, mConsumer);
        *usage |= u;
    }

    return res;
}

ssize_t Camera3SharedOutputStream::getNextSurfaceIdLocked() {
    ssize_t id = -1;
    for (size_t i = 0; i < kMaxOutputs; i++) {
        if (mSurfaceUniqueIds[i].mSurfaceHolder.mSurface == nullptr) {
            id = i;
            break;
        }
    }

    return id;
}

ssize_t Camera3SharedOutputStream::getSurfaceId(const sp<Surface> &surface) {
    Mutex::Autolock l(mLock);
    ssize_t id = -1;
    for (size_t i = 0; i < kMaxOutputs; i++) {
        if (mSurfaceUniqueIds[i].mSurfaceHolder.mSurface == surface) {
            id = i;
            break;
        }
    }

    return id;
}

status_t Camera3SharedOutputStream::getUniqueSurfaceIds(
        const std::vector<size_t>& surfaceIds,
        /*out*/std::vector<size_t>* outUniqueIds) {
    Mutex::Autolock l(mLock);
    if (outUniqueIds == nullptr || surfaceIds.size() > kMaxOutputs) {
        return BAD_VALUE;
    }

    outUniqueIds->clear();
    outUniqueIds->reserve(surfaceIds.size());

    for (const auto& surfaceId : surfaceIds) {
        if (surfaceId >= kMaxOutputs) {
            return BAD_VALUE;
        }
        outUniqueIds->push_back(mSurfaceUniqueIds[surfaceId].mId);
    }
    return OK;
}

status_t Camera3SharedOutputStream::revertPartialUpdateLocked(
        const KeyedVector<size_t, SurfaceHolder> &removedSurfaces,
        const KeyedVector<sp<Surface>, size_t> &attachedSurfaces) {
    status_t ret = OK;

    for (size_t i = 0; i < attachedSurfaces.size(); i++) {
        size_t index = attachedSurfaces.valueAt(i);
        if (mStreamSplitter != nullptr) {
            ret = mStreamSplitter->removeOutput(index);
            if (ret != OK) {
                return UNKNOWN_ERROR;
            }
        }
        mSurfaceUniqueIds[index] = SurfaceHolderUniqueId{mNextUniqueSurfaceId++};
    }

    for (size_t i = 0; i < removedSurfaces.size(); i++) {
        size_t index = removedSurfaces.keyAt(i);
        if (mStreamSplitter != nullptr) {
            ret = mStreamSplitter->addOutput(index, removedSurfaces.valueAt(i).mSurface);
            if (ret != OK) {
                return UNKNOWN_ERROR;
            }
        }
        mSurfaceUniqueIds[index] = SurfaceHolderUniqueId{removedSurfaces.valueAt(i),
                mNextUniqueSurfaceId++};
    }

    return ret;
}

status_t Camera3SharedOutputStream::updateStream(const std::vector<SurfaceHolder> &outputSurfaces,
        const std::vector<OutputStreamInfo> &outputInfo,
        const std::vector<size_t> &removedSurfaceIds,
        KeyedVector<sp<Surface>, size_t> *outputMap) {
    status_t ret = OK;
    Mutex::Autolock l(mLock);

    if ((outputMap == nullptr) || (outputInfo.size() != outputSurfaces.size()) ||
            (outputSurfaces.size() > kMaxOutputs)) {
        return BAD_VALUE;
    }

    uint64_t usage;
    getEndpointUsage(&usage);
    KeyedVector<size_t, SurfaceHolder> removedSurfaces;
    //Check whether the new surfaces are compatible.
    for (const auto &infoIt : outputInfo) {
        bool imgReaderUsage = (infoIt.consumerUsage & GRALLOC_USAGE_SW_READ_OFTEN) ? true : false;
        bool sizeMismatch = ((static_cast<uint32_t>(infoIt.width) != getWidth()) ||
                                (static_cast<uint32_t> (infoIt.height) != getHeight())) ?
                                true : false;
        bool dynamicRangeMismatch = dynamic_range_profile != infoIt.dynamicRangeProfile;
        if ((imgReaderUsage && sizeMismatch) || dynamicRangeMismatch ||
                (infoIt.format != getOriginalFormat() && infoIt.format != getFormat()) ||
                (infoIt.dataSpace != getDataSpace() &&
                 infoIt.dataSpace != getOriginalDataSpace())) {
            ALOGE("%s: Shared surface parameters format: 0x%x dataSpace: 0x%x dynamic range 0x%"
                    PRIx64 " don't match source stream format: 0x%x  dataSpace: 0x%x dynamic"
                    " range 0x%" PRIx64 , __FUNCTION__, infoIt.format, infoIt.dataSpace,
                    infoIt.dynamicRangeProfile, getFormat(), getDataSpace(), dynamic_range_profile);
            return BAD_VALUE;
        }
    }

    //First remove all absent outputs
    for (const auto &it : removedSurfaceIds) {
        if (mStreamSplitter != nullptr) {
            ret = mStreamSplitter->removeOutput(it);
            if (ret != OK) {
                ALOGE("%s: failed with error code %d", __FUNCTION__, ret);
                status_t res = revertPartialUpdateLocked(removedSurfaces, *outputMap);
                if (res != OK) {
                    return res;
                }
                return ret;

            }
        }
        removedSurfaces.add(it, mSurfaceUniqueIds[it].mSurfaceHolder);
        mSurfaceUniqueIds[it] = SurfaceHolderUniqueId{mNextUniqueSurfaceId++};
    }

    //Next add the new outputs
    for (const auto &it : outputSurfaces) {
        ssize_t surfaceId = getNextSurfaceIdLocked();
        if (surfaceId < 0) {
            ALOGE("%s: No more available output slots!", __FUNCTION__);
            status_t res = revertPartialUpdateLocked(removedSurfaces, *outputMap);
            if (res != OK) {
                return res;
            }
            return NO_MEMORY;
        }
        if (mStreamSplitter != nullptr) {
            ret = mStreamSplitter->addOutput(surfaceId, it.mSurface);
            if (ret != OK) {
                ALOGE("%s: failed with error code %d", __FUNCTION__, ret);
                status_t res = revertPartialUpdateLocked(removedSurfaces, *outputMap);
                if (res != OK) {
                    return res;
                }
                return ret;
            }
        }
        mSurfaceUniqueIds[surfaceId] = SurfaceHolderUniqueId{it, mNextUniqueSurfaceId++};
        outputMap->add(it.mSurface, surfaceId);
    }

    return ret;
}

status_t Camera3SharedOutputStream::setTransform(
        int transform, bool mayChangeMirror, int surfaceId) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    status_t res = OK;

    if (surfaceId < 0 || (size_t)surfaceId >= mSurfaceUniqueIds.size()) {
        ALOGE("%s: Invalid surfaceId %d", __FUNCTION__, surfaceId);
        return BAD_VALUE;
    }
    if (transform == -1) return res;

    if (mState == STATE_ERROR) {
        ALOGE("%s: Stream in error state", __FUNCTION__);
        return INVALID_OPERATION;
    }

    auto& surfaceHolderForId = mSurfaceUniqueIds[surfaceId];
    if (surfaceHolderForId.mSurfaceHolder.mMirrorMode != OutputConfiguration::MIRROR_MODE_AUTO &&
            mayChangeMirror) {
        // If the mirroring mode is not AUTO, do not allow transform update
        // which may change mirror.
        return OK;
    }

    surfaceHolderForId.mTransform = transform;
    if (mState == STATE_CONFIGURED) {
        sp<Surface> surface = surfaceHolderForId.mSurfaceHolder.mSurface;
        if (surface != nullptr) {
            res = mStreamSplitter->setTransform(surfaceId, transform);
            if (res != OK) {
                ALOGE("%s: StreamSplitter fails to setTransform: %s(%d)",
                        __FUNCTION__, strerror(-res), res);
                return res;
            }
        }
    }
    return res;
}

} // namespace camera3

} // namespace android
