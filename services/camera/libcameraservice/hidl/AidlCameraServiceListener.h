/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <mutex>
#include <thread>

#include <android/frameworks/cameraservice/common/2.0/types.h>
#include <android/frameworks/cameraservice/service/2.0/ICameraServiceListener.h>
#include <android/frameworks/cameraservice/service/2.1/ICameraServiceListener.h>
#include <android/frameworks/cameraservice/device/2.0/types.h>
#include <android/hardware/BnCameraServiceListener.h>
#include <android/hardware/BpCameraServiceListener.h>

#include <hidl/Status.h>
#include <hidl/CameraHybridInterface.h>

namespace android {
namespace frameworks {
namespace cameraservice {
namespace service {
namespace V2_0 {
namespace implementation {

using hardware::BnCameraServiceListener;
using hardware::BpCameraServiceListener;
using camerahybrid::H2BConverter;
using HCameraDeviceStatus = frameworks::cameraservice::service::V2_0::CameraDeviceStatus;
typedef frameworks::cameraservice::service::V2_0::ICameraServiceListener HCameraServiceListener;

struct H2BCameraServiceListener :
    public H2BConverter<HCameraServiceListener, ICameraServiceListener, BnCameraServiceListener> {
    H2BCameraServiceListener(const sp<HalInterface>& base) : CBase(base) { }

    ~H2BCameraServiceListener() { }

    virtual ::android::binder::Status onStatusChanged(int32_t status,
            const std::string& cameraId, int32_t deviceId) override;
    virtual ::android::binder::Status onPhysicalCameraStatusChanged(int32_t status,
            const std::string& cameraId,
            const std::string& physicalCameraId,
            int32_t deviceId) override;

    virtual ::android::binder::Status onTorchStatusChanged(
            int32_t status, const std::string& cameraId, int32_t deviceId) override;
    virtual ::android::binder::Status onTorchStrengthLevelChanged(
            const std::string& cameraId, int32_t newStrengthLevel, int32_t deviceId) override;
    virtual binder::Status onCameraAccessPrioritiesChanged() {
        // TODO: no implementation yet.
        return binder::Status::ok();
    }
    virtual binder::Status onCameraOpened([[maybe_unused]] const std::string& /*cameraId*/,
            [[maybe_unused]] const std::string& /*clientPackageId*/,
            [[maybe_unused]] int32_t /*deviceId*/) {
        // empty implementation
        return binder::Status::ok();
    }
    virtual binder::Status onCameraClosed([[maybe_unused]] const std::string& /*cameraId*/,
            [[maybe_unused]] int32_t /*deviceId*/) {
        // empty implementation
        return binder::Status::ok();
    }
    virtual binder::Status onCameraOpenedInSharedMode(const std::string& /*cameraId*/,
            const std::string& /*clientPackageId*/, int32_t /*deviceId*/, bool /*primaryClient*/) {
        return binder::Status::ok();
    }
};

} // implementation
} // V2_0
} // service
} // cameraservice
} // frameworks
} // android
