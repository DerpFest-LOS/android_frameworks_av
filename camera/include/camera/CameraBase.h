/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_CAMERA_BASE_H
#define ANDROID_HARDWARE_CAMERA_BASE_H

#include <android/content/AttributionSourceState.h>
#include <android/hardware/ICameraServiceListener.h>

#include <utils/Mutex.h>
#include <binder/BinderService.h>

struct camera_frame_metadata;

namespace android {

namespace hardware {


class ICameraService;
class ICameraServiceListener;

enum {
    /** The facing of the camera is opposite to that of the screen. */
    CAMERA_FACING_BACK = 0,
    /** The facing of the camera is the same as that of the screen. */
    CAMERA_FACING_FRONT = 1,
};

struct CameraInfo : public android::Parcelable {
    /**
     * The direction that the camera faces to. It should be CAMERA_FACING_BACK
     * or CAMERA_FACING_FRONT.
     */
    int facing;

    /**
     * The orientation of the camera image. The value is the angle that the
     * camera image needs to be rotated clockwise so it shows correctly on the
     * display in its natural orientation. It should be 0, 90, 180, or 270.
     *
     * For example, suppose a device has a naturally tall screen. The
     * back-facing camera sensor is mounted in landscape. You are looking at
     * the screen. If the top side of the camera sensor is aligned with the
     * right edge of the screen in natural orientation, the value should be
     * 90. If the top side of a front-facing camera sensor is aligned with the
     * right of the screen, the value should be 270.
     */
    int orientation;

    virtual status_t writeToParcel(android::Parcel* parcel) const;
    virtual status_t readFromParcel(const android::Parcel* parcel);
};

/**
 * Basic status information about a camera device - its id and its current
 * state.
 */
struct CameraStatus : public android::Parcelable {
    /**
     * The app-visible id of the camera device
     */
    std::string cameraId;

    /**
     * Its current status, one of the ICameraService::STATUS_* fields
     */
    int32_t status;

    /**
     * Unavailable physical camera names for a multi-camera device
     */
    std::vector<std::string> unavailablePhysicalIds;

    /**
     * Client package name if camera is open, otherwise not applicable
     */
    std::string clientPackage;

    /**
     * The id of the device owning the camera. For virtual cameras, this is the id of the virtual
     * device owning the camera. For real cameras, this is the default device id, i.e.,
     * kDefaultDeviceId.
     */
    int32_t deviceId;

    virtual status_t writeToParcel(android::Parcel* parcel) const;
    virtual status_t readFromParcel(const android::Parcel* parcel);

    CameraStatus(std::string id, int32_t s, const std::vector<std::string>& unavailSubIds,
            const std::string& clientPkg, int32_t devId) : cameraId(id), status(s),
            unavailablePhysicalIds(unavailSubIds), clientPackage(clientPkg), deviceId(devId) {}
    CameraStatus() : status(ICameraServiceListener::STATUS_PRESENT) {}
};

} // namespace hardware

using content::AttributionSourceState;
using hardware::CameraInfo;

template <typename TCam>
struct CameraTraits {
};

template <typename TCam, typename TCamTraits = CameraTraits<TCam> >
class CameraBase : public IBinder::DeathRecipient
{
public:
    typedef typename TCamTraits::TCamListener       TCamListener;
    typedef typename TCamTraits::TCamUser           TCamUser;
    typedef typename TCamTraits::TCamCallbacks      TCamCallbacks;
    typedef typename TCamTraits::TCamConnectService TCamConnectService;

    static sp<TCam>      connect(int cameraId,
                                 int targetSdkVersion, int rotationOverride, bool forceSlowJpegMode,
                                 const AttributionSourceState &clientAttribution,
                                 int32_t devicePolicy);
    virtual void         disconnect();

    void                 setListener(const sp<TCamListener>& listener);

    static int           getNumberOfCameras(const AttributionSourceState& clientAttribution,
                                            int32_t devicePolicy);

    static status_t      getCameraInfo(int cameraId,
                                       int rotationOverride,
                                       const AttributionSourceState& clientAttribution,
                                       int32_t devicePolicy,
                                       /*out*/
                                       struct hardware::CameraInfo* cameraInfo);

    sp<TCamUser>         remote();

    // Status is set to 'UNKNOWN_ERROR' after successful (re)connection
    status_t             getStatus();

protected:
    CameraBase(int cameraId);
    virtual              ~CameraBase();

    ////////////////////////////////////////////////////////
    // TCamCallbacks implementation
    ////////////////////////////////////////////////////////
    virtual void         notifyCallback(int32_t msgType, int32_t ext,
                                        int32_t ext2);

    ////////////////////////////////////////////////////////
    // Common instance variables
    ////////////////////////////////////////////////////////
    Mutex                            mLock;

    virtual void                     binderDied(const wp<IBinder>& who);

    // helper function to obtain camera service handle
    static const sp<::android::hardware::ICameraService> getCameraService();

    sp<TCamUser>                     mCamera;
    status_t                         mStatus;

    sp<TCamListener>                 mListener;

    const int                        mCameraId;

    typedef CameraBase<TCam>         CameraBaseT;
};

} // namespace android

#endif
