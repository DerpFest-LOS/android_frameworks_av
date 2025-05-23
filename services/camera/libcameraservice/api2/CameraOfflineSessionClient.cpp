/*
 * Copyright (C) 2019 The Android Open Source Project
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

#define LOG_TAG "CameraOfflineClient"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include "CameraOfflineSessionClient.h"
#include <utils/Trace.h>
#include <camera/StringUtils.h>

namespace android {

using binder::Status;

status_t CameraOfflineSessionClient::initialize(sp<CameraProviderManager>, const std::string&) {
    ATRACE_CALL();

    if (mFrameProcessor.get() != nullptr) {
        // Already initialized
        return OK;
    }

    // Verify ops permissions and/or open camera
    auto res = notifyCameraOpening();
    if (res != OK) {
        return res;
    }

    if (mOfflineSession.get() == nullptr) {
        ALOGE("%s: Camera %s: No valid offline session",
                __FUNCTION__, mCameraIdStr.c_str());
        return NO_INIT;
    }

    mFrameProcessor = new camera2::FrameProcessorBase(mOfflineSession);
    std::string threadName = fmt::sprintf("Offline-%s-FrameProc", mCameraIdStr.c_str());
    res = mFrameProcessor->run(threadName.c_str());
    if (res != OK) {
        ALOGE("%s: Unable to start frame processor thread: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    mFrameProcessor->registerListener(camera2::FrameProcessorBase::FRAME_PROCESSOR_LISTENER_MIN_ID,
                                      camera2::FrameProcessorBase::FRAME_PROCESSOR_LISTENER_MAX_ID,
                                      /*listener*/this,
                                      /*sendPartials*/true);

    wp<NotificationListener> weakThis(this);
    res = mOfflineSession->initialize(weakThis);
    if (res != OK) {
        ALOGE("%s: Camera %s: unable to initialize device: %s (%d)",
                __FUNCTION__, mCameraIdStr.c_str(), strerror(-res), res);
        return res;
    }

    for (size_t i = 0; i < mCompositeStreamMap.size(); i++) {
        mCompositeStreamMap.valueAt(i)->switchToOffline();
    }

    return OK;
}

status_t CameraOfflineSessionClient::setCameraServiceWatchdog(bool) {
    return OK;
}

status_t CameraOfflineSessionClient::setRotateAndCropOverride(uint8_t /*rotateAndCrop*/,
        bool /*fromHal*/) {
    // Since we're not submitting more capture requests, changes to rotateAndCrop override
    // make no difference.
    return OK;
}

status_t CameraOfflineSessionClient::setAutoframingOverride(uint8_t) {
    return OK;
}

bool CameraOfflineSessionClient::supportsCameraMute() {
    // Offline mode doesn't support muting
    return false;
}

status_t CameraOfflineSessionClient::setCameraMute(bool) {
    return INVALID_OPERATION;
}

void CameraOfflineSessionClient::setStreamUseCaseOverrides(
        const std::vector<int64_t>& /*useCaseOverrides*/) {
}

void CameraOfflineSessionClient::clearStreamUseCaseOverrides() {
}

bool CameraOfflineSessionClient::supportsZoomOverride() {
    return false;
}

status_t CameraOfflineSessionClient::setZoomOverride(int32_t /*zoomOverride*/) {
    return INVALID_OPERATION;
}

status_t CameraOfflineSessionClient::dump(int fd, const Vector<String16>& args) {
    return BasicClient::dump(fd, args);
}

status_t CameraOfflineSessionClient::dumpClient(int fd, const Vector<String16>& args) {
    std::string result;

    result = "  Offline session dump:\n";
    write(fd, result.c_str(), result.size());

    if (mOfflineSession.get() == nullptr) {
        result = "  *** Offline session is detached\n";
        write(fd, result.c_str(), result.size());
        return NO_ERROR;
    }

    mFrameProcessor->dump(fd, args);

    auto res = mOfflineSession->dump(fd);
    if (res != OK) {
        result = fmt::sprintf("   Error dumping offline session: %s (%d)",
                strerror(-res), res);
        write(fd, result.c_str(), result.size());
    }

    return OK;
}

status_t CameraOfflineSessionClient::startWatchingTags(const std::string &tags, int outFd) {
    return BasicClient::startWatchingTags(tags, outFd);
}

status_t CameraOfflineSessionClient::stopWatchingTags(int outFd) {
    return BasicClient::stopWatchingTags(outFd);
}

status_t CameraOfflineSessionClient::dumpWatchedEventsToVector(std::vector<std::string> &out) {
    return BasicClient::dumpWatchedEventsToVector(out);
}

binder::Status CameraOfflineSessionClient::disconnect() {
    Mutex::Autolock icl(mBinderSerializationLock);

    binder::Status res = Status::ok();
    if (mDisconnected) {
        return res;
    }
    // Allow both client and the media server to disconnect at all times
    int callingPid = getCallingPid();
    if (callingPid != mCallingPid &&
            callingPid != mServicePid) {
        return res;
    }

    mDisconnected = true;

    sCameraService->removeByClient(this);
    sCameraService->logDisconnectedOffline(mCameraIdStr, mCallingPid, getPackageName());

    sp<IBinder> remote = getRemote();
    if (remote != nullptr) {
        remote->unlinkToDeath(sCameraService);
    }

    mFrameProcessor->removeListener(camera2::FrameProcessorBase::FRAME_PROCESSOR_LISTENER_MIN_ID,
                                    camera2::FrameProcessorBase::FRAME_PROCESSOR_LISTENER_MAX_ID,
                                    /*listener*/this);
    mFrameProcessor->requestExit();
    mFrameProcessor->join();

    notifyCameraClosing();
    ALOGI("%s: Disconnected client for offline camera %s for PID %d", __FUNCTION__,
            mCameraIdStr.c_str(), mCallingPid);

    // client shouldn't be able to call into us anymore
    mCallingPid = 0;

    if (mOfflineSession.get() != nullptr) {
        auto ret = mOfflineSession->disconnect();
        if (ret != OK) {
            ALOGE("%s: Failed disconnecting from offline session %s (%d)", __FUNCTION__,
                    strerror(-ret), ret);
        }
        mOfflineSession = nullptr;
    }

    for (size_t i = 0; i < mCompositeStreamMap.size(); i++) {
        auto ret = mCompositeStreamMap.valueAt(i)->deleteInternalStreams();
        if (ret != OK) {
            ALOGE("%s: Failed removing composite stream  %s (%d)", __FUNCTION__,
                    strerror(-ret), ret);
        }
    }
    mCompositeStreamMap.clear();

    return res;
}

void CameraOfflineSessionClient::notifyError(int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    // Thread safe. Don't bother locking.
    // Composites can have multiple internal streams. Error notifications coming from such internal
    // streams may need to remain within camera service.
    bool skipClientNotification = false;
    for (size_t i = 0; i < mCompositeStreamMap.size(); i++) {
        skipClientNotification |= mCompositeStreamMap.valueAt(i)->onError(errorCode, resultExtras);
    }

    if ((mRemoteCallback.get() != nullptr) && (!skipClientNotification)) {
        mRemoteCallback->onDeviceError(errorCode, resultExtras);
    }
}

status_t CameraOfflineSessionClient::notifyCameraOpening() {
    ATRACE_CALL();
    {
        ALOGV("%s: Notify camera opening, package name = %s, client UID = %d", __FUNCTION__,
              getPackageName().c_str(), getClientUid());
    }

    if (mAppOpsManager != nullptr) {
        // Notify app ops that the camera is not available
        mOpsCallback = new OpsCallback(this);
        int32_t res;
        // TODO : possibly change this to OP_OFFLINE_CAMERA_SESSION
        mAppOpsManager->startWatchingMode(AppOpsManager::OP_CAMERA, toString16(getPackageName()),
                                          mOpsCallback);
        // TODO : possibly change this to OP_OFFLINE_CAMERA_SESSION
        res = mAppOpsManager->startOpNoThrow(AppOpsManager::OP_CAMERA, getClientUid(),
                                             toString16(getPackageName()),
                                             /*startIfModeDefault*/ false);

        if (res == AppOpsManager::MODE_ERRORED) {
            ALOGI("Offline Camera %s: Access for \"%s\" has been revoked", mCameraIdStr.c_str(),
                  getPackageName().c_str());
            return PERMISSION_DENIED;
        }

        // If the calling Uid is trusted (a native service), the AppOpsManager could
        // return MODE_IGNORED. Do not treat such case as error.
        if (!mUidIsTrusted && res == AppOpsManager::MODE_IGNORED) {
            ALOGI("Offline Camera %s: Access for \"%s\" has been restricted", mCameraIdStr.c_str(),
                  getPackageName().c_str());
            // Return the same error as for device policy manager rejection
            return -EACCES;
        }
    }

    mCameraOpen = true;

    // Transition device state to OPEN
    sCameraService->mUidPolicy->registerMonitorUid(getClientUid(), /*openCamera*/ true);

    return OK;
}

status_t CameraOfflineSessionClient::notifyCameraClosing() {
    ATRACE_CALL();

    // Check if notifyCameraOpening succeeded, and if so, finish the camera op if necessary
    if (mCameraOpen) {
        // Notify app ops that the camera is available again
        if (mAppOpsManager != nullptr) {
            // TODO : possibly change this to OP_OFFLINE_CAMERA_SESSION
            mAppOpsManager->finishOp(AppOpsManager::OP_CAMERA, getClientUid(),
                                     toString16(getPackageName()));
            mCameraOpen = false;
        }
    }
    // Always stop watching, even if no camera op is active
    if (mOpsCallback != nullptr && mAppOpsManager != nullptr) {
        mAppOpsManager->stopWatchingMode(mOpsCallback);
    }
    mOpsCallback.clear();

    sCameraService->mUidPolicy->unregisterMonitorUid(getClientUid(), /*closeCamera*/ true);

    return OK;
}

void CameraOfflineSessionClient::onResultAvailable(const CaptureResult& result) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    if (mRemoteCallback.get() != NULL) {
        using hardware::camera2::CameraMetadataInfo;
        CameraMetadataInfo resultInfo;
        resultInfo.set<CameraMetadataInfo::metadata>(result.mMetadata);
        mRemoteCallback->onResultReceived(resultInfo, result.mResultExtras,
                result.mPhysicalMetadatas);
    }

    for (size_t i = 0; i < mCompositeStreamMap.size(); i++) {
        mCompositeStreamMap.valueAt(i)->onResultAvailable(result);
    }
}

void CameraOfflineSessionClient::notifyClientSharedAccessPriorityChanged(bool /*primaryClient*/) {
}

void CameraOfflineSessionClient::notifyShutter(const CaptureResultExtras& resultExtras,
        nsecs_t timestamp) {

    if (mRemoteCallback.get() != nullptr) {
        mRemoteCallback->onCaptureStarted(resultExtras, timestamp);
    }

    for (size_t i = 0; i < mCompositeStreamMap.size(); i++) {
        mCompositeStreamMap.valueAt(i)->onShutter(resultExtras, timestamp);
    }
}

status_t CameraOfflineSessionClient::notifyActive(float maxPreviewFps __unused) {
    return startCameraStreamingOps();
}

void CameraOfflineSessionClient::notifyIdle(
        int64_t /*requestCount*/, int64_t /*resultErrorCount*/, bool /*deviceError*/,
        std::pair<int32_t, int32_t> /*mostRequestedFpsRange*/,
        const std::vector<hardware::CameraStreamStats>& /*streamStats*/) {
    if (mRemoteCallback.get() != nullptr) {
        mRemoteCallback->onDeviceIdle();
    }
    finishCameraStreamingOps();
}

void CameraOfflineSessionClient::notifyAutoFocus([[maybe_unused]] uint8_t newState,
                [[maybe_unused]] int triggerId) {
    ALOGV("%s: Autofocus state now %d, last trigger %d",
          __FUNCTION__, newState, triggerId);
}

void CameraOfflineSessionClient::notifyAutoExposure([[maybe_unused]] uint8_t newState,
                [[maybe_unused]] int triggerId) {
    ALOGV("%s: Autoexposure state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
}

void CameraOfflineSessionClient::notifyAutoWhitebalance([[maybe_unused]] uint8_t newState,
                [[maybe_unused]] int triggerId) {
    ALOGV("%s: Auto-whitebalance state now %d, last trigger %d", __FUNCTION__, newState,
            triggerId);
}

void CameraOfflineSessionClient::notifyPrepared(int /*streamId*/) {
    ALOGE("%s: Unexpected stream prepare notification in offline mode!", __FUNCTION__);
    notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DEVICE,
                CaptureResultExtras());
}

void CameraOfflineSessionClient::notifyRequestQueueEmpty() {
    if (mRemoteCallback.get() != nullptr) {
        mRemoteCallback->onRequestQueueEmpty();
    }
}

void CameraOfflineSessionClient::notifyRepeatingRequestError(long /*lastFrameNumber*/) {
    ALOGE("%s: Unexpected repeating request error in offline mode!", __FUNCTION__);
    notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DEVICE,
                CaptureResultExtras());
}

status_t CameraOfflineSessionClient::injectCamera(const std::string& injectedCamId,
            sp<CameraProviderManager> manager) {
    ALOGV("%s: This client doesn't support the injection camera. injectedCamId: %s providerPtr: %p",
            __FUNCTION__, injectedCamId.c_str(), manager.get());

    return OK;
}

status_t CameraOfflineSessionClient::stopInjection() {
    ALOGV("%s: This client doesn't support the injection camera.", __FUNCTION__);

    return OK;
}

status_t CameraOfflineSessionClient::injectSessionParams(
        const hardware::camera2::impl::CameraMetadataNative& sessionParams) {
    ALOGV("%s: This client doesn't support the injecting session parameters camera.",
            __FUNCTION__);
    (void)sessionParams;
    return OK;
}
// ----------------------------------------------------------------------------
}; // namespace android
