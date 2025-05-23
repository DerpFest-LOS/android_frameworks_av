/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_CAPTURERESULT_H
#define ANDROID_HARDWARE_CAPTURERESULT_H

#include <utils/RefBase.h>
#include <binder/Parcelable.h>
#include <camera/CameraMetadata.h>
#include <android/hardware/camera2/CameraMetadataInfo.h>

namespace android {

namespace hardware {
namespace camera2 {
namespace impl {

/**
 * CaptureResultExtras is a structure to encapsulate various indices for a capture result.
 * These indices are framework-internal and not sent to the HAL.
 */
struct CaptureResultExtras : public android::Parcelable {
    /**
     * An integer to index the request sequence that this result belongs to.
     */
    int32_t requestId;

    /**
     * An integer to index this result inside a request sequence, starting from 0.
     */
    int32_t burstId;

    /**
     * TODO: Add documentation for this field.
     */
    int32_t afTriggerId;

    /**
     * TODO: Add documentation for this field.
     */
    int32_t precaptureTriggerId;

    /**
     * A 64bit integer to index the frame number associated with this result.
     */
    int64_t frameNumber;

    /**
     * The partial result count (index) for this capture result.
     */
    int32_t partialResultCount;

    /**
     * For buffer drop errors, the stream ID for the stream that lost a buffer.
     * Otherwise -1.
     */
    int32_t errorStreamId;

    /**
     * For capture result errors, the physical camera ID in case the respective request contains
     * a reference to physical camera device.
     * Empty otherwise.
     */
    std::string errorPhysicalCameraId;

    // The last completed frame numbers shouldn't be checked in onResultReceived() and notifyError()
    // because the output buffers could be arriving after onResultReceived() and
    // notifyError(). Given this constraint, we check it for each
    // onCaptureStarted, and if there is no further onCaptureStarted(),
    // check for onDeviceIdle() to clear out all pending frame numbers.

    /**
     * The latest regular request frameNumber for which all buffers and capture result have been
     * returned or notified as an BUFFER_ERROR/RESULT_ERROR/REQUEST_ERROR. -1 if
     * none has completed.
     */
    int64_t lastCompletedRegularFrameNumber;

    /**
     * The latest reprocess request frameNumber for which all buffers and capture result have been
     * returned or notified as an BUFFER_ERROR/RESULT_ERROR/REQUEST_ERROR. -1 if
     * none has completed.
     */
    int64_t lastCompletedReprocessFrameNumber;

    /**
     * The latest Zsl request frameNumber for which all buffers and capture result have been
     * returned or notified as an BUFFER_ERROR/RESULT_ERROR/REQUEST_ERROR. -1 if
     * none has completed.
     */
    int64_t lastCompletedZslFrameNumber;

    /**
     * Whether the readoutTimestamp variable is valid and should be used.
     */
    bool hasReadoutTimestamp;

    /**
     * The readout timestamp of the capture. Its value is equal to the
     * start-of-exposure timestamp plus the exposure time (and a possible fixed
     * offset due to sensor crop).
     */
    int64_t readoutTimestamp;

    /**
     * Constructor initializes object as invalid by setting requestId to be -1.
     */
    CaptureResultExtras()
        : requestId(-1),
          burstId(0),
          afTriggerId(0),
          precaptureTriggerId(0),
          frameNumber(0),
          partialResultCount(0),
          errorStreamId(-1),
          errorPhysicalCameraId(),
          lastCompletedRegularFrameNumber(-1),
          lastCompletedReprocessFrameNumber(-1),
          lastCompletedZslFrameNumber(-1),
          hasReadoutTimestamp(false),
          readoutTimestamp(0) {
    }

    /**
     * This function returns true if it's a valid CaptureResultExtras object.
     * Otherwise, returns false. It is valid only when requestId is non-negative.
     */
    bool isValid();

    virtual status_t                readFromParcel(const android::Parcel* parcel) override;
    virtual status_t                writeToParcel(android::Parcel* parcel) const override;
};

struct PhysicalCaptureResultInfo : public android::Parcelable {
    using CameraMetadataInfo = android::hardware::camera2::CameraMetadataInfo;
    PhysicalCaptureResultInfo()
        : mPhysicalCameraId(),
          mCameraMetadataInfo() {
    }
    PhysicalCaptureResultInfo(const std::string& cameraId,
            const CameraMetadata& cameraMetadata)
            : mPhysicalCameraId(cameraId) {
              mCameraMetadataInfo.set<CameraMetadataInfo::metadata>(cameraMetadata);
    }

   PhysicalCaptureResultInfo(const std::string& cameraId,
            uint64_t fmqSize)
            : mPhysicalCameraId(cameraId) {
              mCameraMetadataInfo.set<CameraMetadataInfo::fmqSize>(fmqSize);
    }

    std::string mPhysicalCameraId;

    CameraMetadataInfo mCameraMetadataInfo;

    virtual status_t                readFromParcel(const android::Parcel* parcel) override;
    virtual status_t                writeToParcel(android::Parcel* parcel) const override;
};

} // namespace impl
} // namespace camera2
} // namespace hardware

using hardware::camera2::impl::CaptureResultExtras;
using hardware::camera2::impl::PhysicalCaptureResultInfo;

struct CaptureResult : public virtual LightRefBase<CaptureResult> {
    CameraMetadata          mMetadata;
    std::vector<PhysicalCaptureResultInfo> mPhysicalMetadatas;
    CaptureResultExtras     mResultExtras;

    CaptureResult();

    CaptureResult(const CaptureResult& otherResult);

    CaptureResult(CaptureResult &&captureResult);

    status_t                readFromParcel(android::Parcel* parcel);
    status_t                writeToParcel(android::Parcel* parcel) const;
};

}

#endif /* ANDROID_HARDWARE_CAPTURERESULT_H */
