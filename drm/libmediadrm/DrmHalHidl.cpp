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

//#define LOG_NDEBUG 0
#define LOG_TAG "DrmHalHidl"

#include <aidl/android/media/BnResourceManagerClient.h>
#include <android/binder_manager.h>
#include <android/hardware/drm/1.2/types.h>
#include <android/hardware/drm/1.3/IDrmFactory.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <hidl/ServiceManagement.h>
#include <media/EventMetric.h>
#include <media/MediaMetrics.h>
#include <media/PluginMetricsReporting.h>
#include <media/drm/DrmAPI.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/base64.h>
#include <media/stagefright/foundation/hexdump.h>
#include <mediadrm/DrmHalHidl.h>
#include <mediadrm/DrmSessionClientInterface.h>
#include <mediadrm/DrmSessionManager.h>
#include <mediadrm/DrmStatus.h>
#include <mediadrm/DrmUtils.h>
#include <mediadrm/IDrmMetricsConsumer.h>
#include <utils/Log.h>

#include <iomanip>
#include <vector>

using ::android::sp;
using ::android::DrmUtils::toStatusT;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::drm::V1_1::DrmMetricGroup;
using ::android::os::PersistableBundle;
using drm::V1_0::KeyedVector;
using drm::V1_0::KeyRequestType;
using drm::V1_0::KeyType;
using drm::V1_0::KeyValue;
using drm::V1_0::SecureStop;
using drm::V1_0::SecureStopId;
using drm::V1_0::Status;
using drm::V1_1::HdcpLevel;
using drm::V1_1::SecureStopRelease;
using drm::V1_1::SecurityLevel;
using drm::V1_2::KeySetId;
using drm::V1_2::KeyStatusType;

typedef drm::V1_1::KeyRequestType KeyRequestType_V1_1;
typedef drm::V1_2::Status Status_V1_2;
typedef drm::V1_2::HdcpLevel HdcpLevel_V1_2;

namespace {

// This constant corresponds to the PROPERTY_DEVICE_UNIQUE_ID constant
// in the MediaDrm API.
constexpr char kPropertyDeviceUniqueId[] = "deviceUniqueId";
constexpr char kEqualsSign[] = "=";

template <typename T>
std::string toBase64StringNoPad(const T* data, size_t size) {
    // Note that the base 64 conversion only works with arrays of single-byte
    // values. If the source is empty or is not an array of single-byte values,
    // return empty string.
    if (size == 0 || sizeof(data[0]) != 1) {
        return "";
    }

    android::AString outputString;
    encodeBase64(data, size, &outputString);
    // Remove trailing equals padding if it exists.
    while (outputString.size() > 0 && outputString.endsWith(kEqualsSign)) {
        outputString.erase(outputString.size() - 1, 1);
    }

    return std::string(outputString.c_str(), outputString.size());
}

}  // anonymous namespace

namespace android {

#define INIT_CHECK()                             \
    {                                            \
        if (mInitCheck != OK) return mInitCheck; \
    }

static const Vector<uint8_t> toVector(const hidl_vec<uint8_t>& vec) {
    Vector<uint8_t> vector;
    vector.appendArray(vec.data(), vec.size());
    return *const_cast<const Vector<uint8_t>*>(&vector);
}

static hidl_vec<uint8_t> toHidlVec(const Vector<uint8_t>& vector) {
    hidl_vec<uint8_t> vec;
    vec.setToExternal(const_cast<uint8_t*>(vector.array()), vector.size());
    return vec;
}

static String8 toString8(const hidl_string& string) {
    return String8(string.c_str());
}

static hidl_string toHidlString(const String8& string) {
    return hidl_string(string.c_str());
}

static DrmPlugin::SecurityLevel toSecurityLevel(SecurityLevel level) {
    switch (level) {
        case SecurityLevel::SW_SECURE_CRYPTO:
            return DrmPlugin::kSecurityLevelSwSecureCrypto;
        case SecurityLevel::SW_SECURE_DECODE:
            return DrmPlugin::kSecurityLevelSwSecureDecode;
        case SecurityLevel::HW_SECURE_CRYPTO:
            return DrmPlugin::kSecurityLevelHwSecureCrypto;
        case SecurityLevel::HW_SECURE_DECODE:
            return DrmPlugin::kSecurityLevelHwSecureDecode;
        case SecurityLevel::HW_SECURE_ALL:
            return DrmPlugin::kSecurityLevelHwSecureAll;
        default:
            return DrmPlugin::kSecurityLevelUnknown;
    }
}

static SecurityLevel toHidlSecurityLevel(DrmPlugin::SecurityLevel level) {
    switch (level) {
        case DrmPlugin::kSecurityLevelSwSecureCrypto:
            return SecurityLevel::SW_SECURE_CRYPTO;
        case DrmPlugin::kSecurityLevelSwSecureDecode:
            return SecurityLevel::SW_SECURE_DECODE;
        case DrmPlugin::kSecurityLevelHwSecureCrypto:
            return SecurityLevel::HW_SECURE_CRYPTO;
        case DrmPlugin::kSecurityLevelHwSecureDecode:
            return SecurityLevel::HW_SECURE_DECODE;
        case DrmPlugin::kSecurityLevelHwSecureAll:
            return SecurityLevel::HW_SECURE_ALL;
        default:
            return SecurityLevel::UNKNOWN;
    }
}

static DrmPlugin::OfflineLicenseState toOfflineLicenseState(OfflineLicenseState licenseState) {
    switch (licenseState) {
        case OfflineLicenseState::USABLE:
            return DrmPlugin::kOfflineLicenseStateUsable;
        case OfflineLicenseState::INACTIVE:
            return DrmPlugin::kOfflineLicenseStateReleased;
        default:
            return DrmPlugin::kOfflineLicenseStateUnknown;
    }
}

static DrmPlugin::HdcpLevel toHdcpLevel(HdcpLevel_V1_2 level) {
    switch (level) {
        case HdcpLevel_V1_2::HDCP_NONE:
            return DrmPlugin::kHdcpNone;
        case HdcpLevel_V1_2::HDCP_V1:
            return DrmPlugin::kHdcpV1;
        case HdcpLevel_V1_2::HDCP_V2:
            return DrmPlugin::kHdcpV2;
        case HdcpLevel_V1_2::HDCP_V2_1:
            return DrmPlugin::kHdcpV2_1;
        case HdcpLevel_V1_2::HDCP_V2_2:
            return DrmPlugin::kHdcpV2_2;
        case HdcpLevel_V1_2::HDCP_V2_3:
            return DrmPlugin::kHdcpV2_3;
        case HdcpLevel_V1_2::HDCP_NO_OUTPUT:
            return DrmPlugin::kHdcpNoOutput;
        default:
            return DrmPlugin::kHdcpLevelUnknown;
    }
}
static ::KeyedVector toHidlKeyedVector(const KeyedVector<String8, String8>& keyedVector) {
    std::vector<KeyValue> stdKeyedVector;
    for (size_t i = 0; i < keyedVector.size(); i++) {
        KeyValue keyValue;
        keyValue.key = toHidlString(keyedVector.keyAt(i));
        keyValue.value = toHidlString(keyedVector.valueAt(i));
        stdKeyedVector.push_back(keyValue);
    }
    return ::KeyedVector(stdKeyedVector);
}

static KeyedVector<String8, String8> toKeyedVector(const ::KeyedVector& hKeyedVector) {
    KeyedVector<String8, String8> keyedVector;
    for (size_t i = 0; i < hKeyedVector.size(); i++) {
        keyedVector.add(toString8(hKeyedVector[i].key), toString8(hKeyedVector[i].value));
    }
    return keyedVector;
}

static List<Vector<uint8_t>> toSecureStops(const hidl_vec<SecureStop>& hSecureStops) {
    List<Vector<uint8_t>> secureStops;
    for (size_t i = 0; i < hSecureStops.size(); i++) {
        secureStops.push_back(toVector(hSecureStops[i].opaqueData));
    }
    return secureStops;
}

static List<Vector<uint8_t>> toSecureStopIds(const hidl_vec<SecureStopId>& hSecureStopIds) {
    List<Vector<uint8_t>> secureStopIds;
    for (size_t i = 0; i < hSecureStopIds.size(); i++) {
        secureStopIds.push_back(toVector(hSecureStopIds[i]));
    }
    return secureStopIds;
}

static List<Vector<uint8_t>> toKeySetIds(const hidl_vec<KeySetId>& hKeySetIds) {
    List<Vector<uint8_t>> keySetIds;
    for (size_t i = 0; i < hKeySetIds.size(); i++) {
        keySetIds.push_back(toVector(hKeySetIds[i]));
    }
    return keySetIds;
}

Mutex DrmHalHidl::mLock;

struct DrmHalHidl::DrmSessionClient : public aidl::android::media::BnResourceManagerClient {
    explicit DrmSessionClient(DrmHalHidl* drm, const Vector<uint8_t>& sessionId)
        : mSessionId(sessionId), mDrm(drm) {}

    ::ndk::ScopedAStatus reclaimResource(bool* _aidl_return) override;
    ::ndk::ScopedAStatus getName(::std::string* _aidl_return) override;

    const Vector<uint8_t> mSessionId;

    virtual ~DrmSessionClient();

  private:
    wp<DrmHalHidl> mDrm;

    DISALLOW_EVIL_CONSTRUCTORS(DrmSessionClient);
};

::ndk::ScopedAStatus DrmHalHidl::DrmSessionClient::reclaimResource(bool* _aidl_return) {
    auto sessionId = mSessionId;
    sp<DrmHalHidl> drm = mDrm.promote();
    if (drm == NULL) {
        *_aidl_return = true;
        return ::ndk::ScopedAStatus::ok();
    }
    status_t err = drm->closeSession(sessionId);
    if (err != OK) {
        *_aidl_return = false;
        return ::ndk::ScopedAStatus::ok();
    }
    drm->sendEvent(EventType::SESSION_RECLAIMED, toHidlVec(sessionId), hidl_vec<uint8_t>());
    *_aidl_return = true;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus DrmHalHidl::DrmSessionClient::getName(::std::string* _aidl_return) {
    String8 name;
    sp<DrmHalHidl> drm = mDrm.promote();
    if (drm == NULL) {
        name.append("<deleted>");
    } else if (drm->getPropertyStringInternal(String8("vendor"), name) != OK || name.empty()) {
        name.append("<Get vendor failed or is empty>");
    }
    name.append("[");
    for (size_t i = 0; i < mSessionId.size(); ++i) {
        name.appendFormat("%02x", mSessionId[i]);
    }
    name.append("]");
    *_aidl_return = name;
    return ::ndk::ScopedAStatus::ok();
}

DrmHalHidl::DrmSessionClient::~DrmSessionClient() {
    DrmSessionManager::Instance()->removeSession(mSessionId);
}

DrmHalHidl::DrmHalHidl()
    : mFactories(makeDrmFactories()),
      mInitCheck((mFactories.size() == 0) ? ERROR_UNSUPPORTED : NO_INIT) {}

void DrmHalHidl::closeOpenSessions() {
    Mutex::Autolock autoLock(mLock);
    auto openSessions = mOpenSessions;
    for (size_t i = 0; i < openSessions.size(); i++) {
        mLock.unlock();
        closeSession(openSessions[i]->mSessionId);
        mLock.lock();
    }
    mOpenSessions.clear();
}

DrmHalHidl::~DrmHalHidl() {}

void DrmHalHidl::cleanup() {
    closeOpenSessions();

    Mutex::Autolock autoLock(mLock);
    if (mInitCheck == OK) reportFrameworkMetrics(reportPluginMetrics());

    setListener(NULL);
    mInitCheck = NO_INIT;
    if (mPluginV1_2 != NULL) {
        if (!mPluginV1_2->setListener(NULL).isOk()) {
            mInitCheck = DEAD_OBJECT;
        }
    } else if (mPlugin != NULL) {
        if (!mPlugin->setListener(NULL).isOk()) {
            mInitCheck = DEAD_OBJECT;
        }
    }
    mPlugin.clear();
    mPluginV1_1.clear();
    mPluginV1_2.clear();
    mPluginV1_4.clear();
}

std::vector<sp<IDrmFactory>> DrmHalHidl::makeDrmFactories() {
    static std::vector<sp<IDrmFactory>> factories(DrmUtils::MakeDrmFactories());
    if (factories.size() == 0) {
        DrmUtils::LOG2BI("No hidl drm factories found");
        // could be in passthrough mode, load the default passthrough service
        auto passthrough = IDrmFactory::getService();
        if (passthrough != NULL) {
            DrmUtils::LOG2BI("makeDrmFactories: using default passthrough drm instance");
            factories.push_back(passthrough);
        } else {
            DrmUtils::LOG2BW("Failed to find passthrough drm factories");
        }
    }
    return factories;
}

sp<IDrmPlugin> DrmHalHidl::makeDrmPlugin(const sp<IDrmFactory>& factory, const uint8_t uuid[16],
                                         const String8& appPackageName) {
    mAppPackageName = appPackageName;
    mMetrics.SetAppPackageName(appPackageName);
    mMetrics.SetAppUid(AIBinder_getCallingUid());

    sp<IDrmPlugin> plugin;
    Return<void> hResult = factory->createPlugin(
            uuid, appPackageName.c_str(), [&](Status status, const sp<IDrmPlugin>& hPlugin) {
                if (status != Status::OK) {
                    DrmUtils::LOG2BE(uuid, "Failed to make drm plugin: %d", status);
                    return;
                }
                plugin = hPlugin;
            });

    if (!hResult.isOk()) {
        DrmUtils::LOG2BE(uuid, "createPlugin remote call failed: %s",
                         hResult.description().c_str());
    }

    return plugin;
}

DrmStatus DrmHalHidl::initCheck() const {
    return DrmStatus(mInitCheck);
}

DrmStatus DrmHalHidl::setListener(const sp<IDrmClient>& listener) {
    Mutex::Autolock lock(mEventLock);
    mListener = listener;
    return DrmStatus(NO_ERROR);
}

Return<void> DrmHalHidl::sendEvent(EventType hEventType, const hidl_vec<uint8_t>& sessionId,
                                   const hidl_vec<uint8_t>& data) {
    mMetrics.mEventCounter.Increment((uint32_t)hEventType);

    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Mutex::Autolock lock(mNotifyLock);
        DrmPlugin::EventType eventType;
        switch (hEventType) {
            case EventType::PROVISION_REQUIRED:
                eventType = DrmPlugin::kDrmPluginEventProvisionRequired;
                break;
            case EventType::KEY_NEEDED:
                eventType = DrmPlugin::kDrmPluginEventKeyNeeded;
                break;
            case EventType::KEY_EXPIRED:
                eventType = DrmPlugin::kDrmPluginEventKeyExpired;
                break;
            case EventType::VENDOR_DEFINED:
                eventType = DrmPlugin::kDrmPluginEventVendorDefined;
                break;
            case EventType::SESSION_RECLAIMED:
                eventType = DrmPlugin::kDrmPluginEventSessionReclaimed;
                break;
            default:
                return Void();
        }
        listener->sendEvent(eventType, sessionId, data);
    }
    return Void();
}

Return<void> DrmHalHidl::sendExpirationUpdate(const hidl_vec<uint8_t>& sessionId,
                                              int64_t expiryTimeInMS) {
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Mutex::Autolock lock(mNotifyLock);
        listener->sendExpirationUpdate(sessionId, expiryTimeInMS);
    }
    return Void();
}

Return<void> DrmHalHidl::sendKeysChange(const hidl_vec<uint8_t>& sessionId,
                                        const hidl_vec<KeyStatus_V1_0>& keyStatusList_V1_0,
                                        bool hasNewUsableKey) {
    std::vector<KeyStatus> keyStatusVec;
    for (const auto& keyStatus_V1_0 : keyStatusList_V1_0) {
        keyStatusVec.push_back(
                {keyStatus_V1_0.keyId, static_cast<KeyStatusType>(keyStatus_V1_0.type)});
    }
    hidl_vec<KeyStatus> keyStatusList_V1_2(keyStatusVec);
    return sendKeysChange_1_2(sessionId, keyStatusList_V1_2, hasNewUsableKey);
}

Return<void> DrmHalHidl::sendKeysChange_1_2(const hidl_vec<uint8_t>& sessionId,
                                            const hidl_vec<KeyStatus>& hKeyStatusList,
                                            bool hasNewUsableKey) {
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        std::vector<DrmKeyStatus> keyStatusList;
        size_t nKeys = hKeyStatusList.size();
        for (size_t i = 0; i < nKeys; ++i) {
            const KeyStatus& keyStatus = hKeyStatusList[i];
            uint32_t type;
            switch (keyStatus.type) {
                case KeyStatusType::USABLE:
                    type = DrmPlugin::kKeyStatusType_Usable;
                    break;
                case KeyStatusType::EXPIRED:
                    type = DrmPlugin::kKeyStatusType_Expired;
                    break;
                case KeyStatusType::OUTPUTNOTALLOWED:
                    type = DrmPlugin::kKeyStatusType_OutputNotAllowed;
                    break;
                case KeyStatusType::STATUSPENDING:
                    type = DrmPlugin::kKeyStatusType_StatusPending;
                    break;
                case KeyStatusType::USABLEINFUTURE:
                    type = DrmPlugin::kKeyStatusType_UsableInFuture;
                    break;
                case KeyStatusType::INTERNALERROR:
                default:
                    type = DrmPlugin::kKeyStatusType_InternalError;
                    break;
            }
            keyStatusList.push_back({type, keyStatus.keyId});
            mMetrics.mKeyStatusChangeCounter.Increment((uint32_t)keyStatus.type);
        }

        Mutex::Autolock lock(mNotifyLock);
        listener->sendKeysChange(sessionId, keyStatusList, hasNewUsableKey);
    } else {
        // There's no listener. But we still want to count the key change
        // events.
        size_t nKeys = hKeyStatusList.size();
        for (size_t i = 0; i < nKeys; i++) {
            mMetrics.mKeyStatusChangeCounter.Increment((uint32_t)hKeyStatusList[i].type);
        }
    }

    return Void();
}

Return<void> DrmHalHidl::sendSessionLostState(const hidl_vec<uint8_t>& sessionId) {
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Mutex::Autolock lock(mNotifyLock);
        listener->sendSessionLostState(sessionId);
    }
    return Void();
}

DrmStatus DrmHalHidl::matchMimeTypeAndSecurityLevel(const sp<IDrmFactory>& factory,
                                                    const uint8_t uuid[16], const String8& mimeType,
                                                    DrmPlugin::SecurityLevel level,
                                                    bool* isSupported) {
    *isSupported = false;

    // handle default value cases
    if (level == DrmPlugin::kSecurityLevelUnknown) {
        if (mimeType == "") {
            // isCryptoSchemeSupported(uuid)
            *isSupported = true;
            return DrmStatus(OK);
        }
        // isCryptoSchemeSupported(uuid, mimeType)
        auto hResult = factory->isContentTypeSupported(mimeType.c_str());
        if (!hResult.isOk()) {
            return DrmStatus(DEAD_OBJECT);
        }
        *isSupported = hResult;
        return DrmStatus(OK);
    } else if (mimeType == "") {
        return DrmStatus(BAD_VALUE);
    }

    sp<drm::V1_2::IDrmFactory> factoryV1_2 = drm::V1_2::IDrmFactory::castFrom(factory);
    if (factoryV1_2 == NULL) {
        return DrmStatus(ERROR_UNSUPPORTED);
    } else {
        auto hResult = factoryV1_2->isCryptoSchemeSupported_1_2(uuid, mimeType.c_str(),
                                                                toHidlSecurityLevel(level));
        if (!hResult.isOk()) {
            return DrmStatus(DEAD_OBJECT);
        }
        *isSupported = hResult;
        return DrmStatus(OK);
    }
}

DrmStatus DrmHalHidl::isCryptoSchemeSupported(const uint8_t uuid[16], const String8& mimeType,
                                              DrmPlugin::SecurityLevel level, bool* isSupported) {
    Mutex::Autolock autoLock(mLock);
    *isSupported = false;
    for (ssize_t i = mFactories.size() - 1; i >= 0; i--) {
        auto hResult = mFactories[i]->isCryptoSchemeSupported(uuid);
        if (hResult.isOk() && hResult) {
            return matchMimeTypeAndSecurityLevel(mFactories[i], uuid, mimeType, level, isSupported);
        }
    }
    return DrmStatus(OK);
}

DrmStatus DrmHalHidl::createPlugin(const uint8_t uuid[16], const String8& appPackageName) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck == ERROR_UNSUPPORTED) return mInitCheck;
    for (ssize_t i = mFactories.size() - 1; i >= 0; i--) {
        auto hResult = mFactories[i]->isCryptoSchemeSupported(uuid);
        if (hResult.isOk() && hResult) {
            auto plugin = makeDrmPlugin(mFactories[i], uuid, appPackageName);
            if (plugin != NULL) {
                mPlugin = plugin;
                mPluginV1_1 = drm::V1_1::IDrmPlugin::castFrom(mPlugin);
                mPluginV1_2 = drm::V1_2::IDrmPlugin::castFrom(mPlugin);
                mPluginV1_4 = drm::V1_4::IDrmPlugin::castFrom(mPlugin);
                break;
            }
        }
    }

    if (mPlugin == NULL) {
        DrmUtils::LOG2BE(uuid, "No supported hal instance found");
        mInitCheck = ERROR_UNSUPPORTED;
    } else {
        mInitCheck = OK;
        if (mPluginV1_2 != NULL) {
            if (!mPluginV1_2->setListener(this).isOk()) {
                mInitCheck = DEAD_OBJECT;
            }
        } else if (!mPlugin->setListener(this).isOk()) {
            mInitCheck = DEAD_OBJECT;
        }
        if (mInitCheck != OK) {
            mPlugin.clear();
            mPluginV1_1.clear();
            mPluginV1_2.clear();
            mPluginV1_4.clear();
        }
    }

    return DrmStatus(mInitCheck);
}

DrmStatus DrmHalHidl::destroyPlugin() {
    cleanup();
    return DrmStatus(OK);
}

DrmStatus DrmHalHidl::openSession(DrmPlugin::SecurityLevel level, Vector<uint8_t>& sessionId) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    SecurityLevel hSecurityLevel = toHidlSecurityLevel(level);
    bool setSecurityLevel = true;

    if (level == DrmPlugin::kSecurityLevelMax) {
        setSecurityLevel = false;
    } else {
        if (hSecurityLevel == SecurityLevel::UNKNOWN) {
            return ERROR_DRM_CANNOT_HANDLE;
        }
    }

    DrmStatus err = UNKNOWN_ERROR;
    bool retry = true;
    do {
        hidl_vec<uint8_t> hSessionId;

        Return<void> hResult;
        if (mPluginV1_1 == NULL || !setSecurityLevel) {
            hResult = mPlugin->openSession([&](Status status, const hidl_vec<uint8_t>& id) {
                if (status == Status::OK) {
                    sessionId = toVector(id);
                }
                err = toStatusT(status);
            });
        } else {
            hResult = mPluginV1_1->openSession_1_1(hSecurityLevel,
                                                   [&](Status status, const hidl_vec<uint8_t>& id) {
                                                       if (status == Status::OK) {
                                                           sessionId = toVector(id);
                                                       }
                                                       err = toStatusT(status);
                                                   });
        }

        if (!hResult.isOk()) {
            err = DEAD_OBJECT;
        }

        if (err == ERROR_DRM_RESOURCE_BUSY && retry) {
            mLock.unlock();
            // reclaimSession may call back to closeSession, since mLock is
            // shared between Drm instances, we should unlock here to avoid
            // deadlock.
            retry = DrmSessionManager::Instance()->reclaimSession(AIBinder_getCallingPid());
            mLock.lock();
        } else {
            retry = false;
        }
    } while (retry);

    if (err == OK) {
        std::shared_ptr<DrmSessionClient> client =
                ndk::SharedRefBase::make<DrmSessionClient>(this, sessionId);
        DrmSessionManager::Instance()->addSession(
                AIBinder_getCallingPid(), std::static_pointer_cast<IResourceManagerClient>(client),
                sessionId);
        mOpenSessions.push_back(client);
        mMetrics.SetSessionStart(sessionId);
    }

    mMetrics.mOpenSessionCounter.Increment(err);
    return err;
}

DrmStatus DrmHalHidl::closeSession(Vector<uint8_t> const& sessionId) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status = mPlugin->closeSession(toHidlVec(sessionId));
    if (status.isOk()) {
        if (status == Status::OK) {
            DrmSessionManager::Instance()->removeSession(sessionId);
            for (auto i = mOpenSessions.begin(); i != mOpenSessions.end(); i++) {
                if (isEqualSessionId((*i)->mSessionId, sessionId)) {
                    mOpenSessions.erase(i);
                    break;
                }
            }
        }
        DrmStatus response = toStatusT(status);
        mMetrics.SetSessionEnd(sessionId);
        mMetrics.mCloseSessionCounter.Increment(response);
        return response;
    }
    mMetrics.mCloseSessionCounter.Increment(DEAD_OBJECT);
    return DrmStatus(DEAD_OBJECT);
}

static DrmPlugin::KeyRequestType toKeyRequestType(KeyRequestType keyRequestType) {
    switch (keyRequestType) {
        case KeyRequestType::INITIAL:
            return DrmPlugin::kKeyRequestType_Initial;
            break;
        case KeyRequestType::RENEWAL:
            return DrmPlugin::kKeyRequestType_Renewal;
            break;
        case KeyRequestType::RELEASE:
            return DrmPlugin::kKeyRequestType_Release;
            break;
        default:
            return DrmPlugin::kKeyRequestType_Unknown;
            break;
    }
}

static DrmPlugin::KeyRequestType toKeyRequestType_1_1(KeyRequestType_V1_1 keyRequestType) {
    switch (keyRequestType) {
        case KeyRequestType_V1_1::NONE:
            return DrmPlugin::kKeyRequestType_None;
            break;
        case KeyRequestType_V1_1::UPDATE:
            return DrmPlugin::kKeyRequestType_Update;
            break;
        default:
            return toKeyRequestType(static_cast<KeyRequestType>(keyRequestType));
            break;
    }
}

DrmStatus DrmHalHidl::getKeyRequest(Vector<uint8_t> const& sessionId,
                                    Vector<uint8_t> const& initData, String8 const& mimeType,
                                    DrmPlugin::KeyType keyType,
                                    KeyedVector<String8, String8> const& optionalParameters,
                                    Vector<uint8_t>& request, String8& defaultUrl,
                                    DrmPlugin::KeyRequestType* keyRequestType) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();
    EventTimer<status_t> keyRequestTimer(&mMetrics.mGetKeyRequestTimeUs);

    DrmSessionManager::Instance()->useSession(sessionId);

    KeyType hKeyType;
    if (keyType == DrmPlugin::kKeyType_Streaming) {
        hKeyType = KeyType::STREAMING;
    } else if (keyType == DrmPlugin::kKeyType_Offline) {
        hKeyType = KeyType::OFFLINE;
    } else if (keyType == DrmPlugin::kKeyType_Release) {
        hKeyType = KeyType::RELEASE;
    } else {
        keyRequestTimer.SetAttribute(BAD_VALUE);
        return BAD_VALUE;
    }

    ::KeyedVector hOptionalParameters = toHidlKeyedVector(optionalParameters);

    DrmStatus err = UNKNOWN_ERROR;
    Return<void> hResult;

    if (mPluginV1_2 != NULL) {
        hResult = mPluginV1_2->getKeyRequest_1_2(
                toHidlVec(sessionId), toHidlVec(initData), toHidlString(mimeType), hKeyType,
                hOptionalParameters,
                [&](Status_V1_2 status, const hidl_vec<uint8_t>& hRequest,
                    KeyRequestType_V1_1 hKeyRequestType, const hidl_string& hDefaultUrl) {
                    if (status == Status_V1_2::OK) {
                        request = toVector(hRequest);
                        defaultUrl = toString8(hDefaultUrl);
                        *keyRequestType = toKeyRequestType_1_1(hKeyRequestType);
                    }
                    err = toStatusT(status);
                });
    } else if (mPluginV1_1 != NULL) {
        hResult = mPluginV1_1->getKeyRequest_1_1(
                toHidlVec(sessionId), toHidlVec(initData), toHidlString(mimeType), hKeyType,
                hOptionalParameters,
                [&](Status status, const hidl_vec<uint8_t>& hRequest,
                    KeyRequestType_V1_1 hKeyRequestType, const hidl_string& hDefaultUrl) {
                    if (status == Status::OK) {
                        request = toVector(hRequest);
                        defaultUrl = toString8(hDefaultUrl);
                        *keyRequestType = toKeyRequestType_1_1(hKeyRequestType);
                    }
                    err = toStatusT(status);
                });
    } else {
        hResult = mPlugin->getKeyRequest(
                toHidlVec(sessionId), toHidlVec(initData), toHidlString(mimeType), hKeyType,
                hOptionalParameters,
                [&](Status status, const hidl_vec<uint8_t>& hRequest,
                    KeyRequestType hKeyRequestType, const hidl_string& hDefaultUrl) {
                    if (status == Status::OK) {
                        request = toVector(hRequest);
                        defaultUrl = toString8(hDefaultUrl);
                        *keyRequestType = toKeyRequestType(hKeyRequestType);
                    }
                    err = toStatusT(status);
                });
    }

    err = hResult.isOk() ? err : DEAD_OBJECT;
    keyRequestTimer.SetAttribute(err);
    return err;
}

DrmStatus DrmHalHidl::provideKeyResponse(Vector<uint8_t> const& sessionId,
                                         Vector<uint8_t> const& response,
                                         Vector<uint8_t>& keySetId) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();
    EventTimer<status_t> keyResponseTimer(&mMetrics.mProvideKeyResponseTimeUs);

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult =
            mPlugin->provideKeyResponse(toHidlVec(sessionId), toHidlVec(response),
                                        [&](Status status, const hidl_vec<uint8_t>& hKeySetId) {
                                            if (status == Status::OK) {
                                                keySetId = toVector(hKeySetId);
                                            }
                                            err = toStatusT(status);
                                        });
    err = hResult.isOk() ? err : DEAD_OBJECT;
    keyResponseTimer.SetAttribute(err);
    return err;
}

DrmStatus DrmHalHidl::removeKeys(Vector<uint8_t> const& keySetId) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status = mPlugin->removeKeys(toHidlVec(keySetId));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::restoreKeys(Vector<uint8_t> const& sessionId,
                                  Vector<uint8_t> const& keySetId) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    Return<Status> status = mPlugin->restoreKeys(toHidlVec(sessionId), toHidlVec(keySetId));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::queryKeyStatus(Vector<uint8_t> const& sessionId,
                                     KeyedVector<String8, String8>& infoMap) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    ::KeyedVector hInfoMap;

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->queryKeyStatus(
            toHidlVec(sessionId), [&](Status status, const hidl_vec<KeyValue>& map) {
                if (status == Status::OK) {
                    infoMap = toKeyedVector(map);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? DrmStatus(err) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getProvisionRequest(String8 const& certType, String8 const& certAuthority,
                                          Vector<uint8_t>& request, String8& defaultUrl) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;
    Return<void> hResult;

    if (mPluginV1_2 != NULL) {
        hResult = mPluginV1_2->getProvisionRequest_1_2(
                toHidlString(certType), toHidlString(certAuthority),
                [&](Status_V1_2 status, const hidl_vec<uint8_t>& hRequest,
                    const hidl_string& hDefaultUrl) {
                    if (status == Status_V1_2::OK) {
                        request = toVector(hRequest);
                        defaultUrl = toString8(hDefaultUrl);
                    }
                    err = toStatusT(status);
                });
    } else {
        hResult = mPlugin->getProvisionRequest(toHidlString(certType), toHidlString(certAuthority),
                                               [&](Status status, const hidl_vec<uint8_t>& hRequest,
                                                   const hidl_string& hDefaultUrl) {
                                                   if (status == Status::OK) {
                                                       request = toVector(hRequest);
                                                       defaultUrl = toString8(hDefaultUrl);
                                                   }
                                                   err = toStatusT(status);
                                               });
    }

    err = hResult.isOk() ? err : DEAD_OBJECT;
    mMetrics.mGetProvisionRequestCounter.Increment(err);
    return err;
}

DrmStatus DrmHalHidl::provideProvisionResponse(Vector<uint8_t> const& response,
                                               Vector<uint8_t>& certificate,
                                               Vector<uint8_t>& wrappedKey) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->provideProvisionResponse(
            toHidlVec(response), [&](Status status, const hidl_vec<uint8_t>& hCertificate,
                                     const hidl_vec<uint8_t>& hWrappedKey) {
                if (status == Status::OK) {
                    certificate = toVector(hCertificate);
                    wrappedKey = toVector(hWrappedKey);
                }
                err = toStatusT(status);
            });

    err = hResult.isOk() ? err : DEAD_OBJECT;
    mMetrics.mProvideProvisionResponseCounter.Increment(err);
    return err;
}

DrmStatus DrmHalHidl::getSecureStops(List<Vector<uint8_t>>& secureStops) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult =
            mPlugin->getSecureStops([&](Status status, const hidl_vec<SecureStop>& hSecureStops) {
                if (status == Status::OK) {
                    secureStops = toSecureStops(hSecureStops);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getSecureStopIds(List<Vector<uint8_t>>& secureStopIds) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPluginV1_1 == NULL) {
        return ERROR_DRM_CANNOT_HANDLE;
    }

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPluginV1_1->getSecureStopIds(
            [&](Status status, const hidl_vec<SecureStopId>& hSecureStopIds) {
                if (status == Status::OK) {
                    secureStopIds = toSecureStopIds(hSecureStopIds);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getSecureStop(Vector<uint8_t> const& ssid, Vector<uint8_t>& secureStop) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->getSecureStop(
            toHidlVec(ssid), [&](Status status, const SecureStop& hSecureStop) {
                if (status == Status::OK) {
                    secureStop = toVector(hSecureStop.opaqueData);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::releaseSecureStops(Vector<uint8_t> const& ssRelease) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status(Status::ERROR_DRM_UNKNOWN);
    if (mPluginV1_1 != NULL) {
        SecureStopRelease secureStopRelease;
        secureStopRelease.opaqueData = toHidlVec(ssRelease);
        status = mPluginV1_1->releaseSecureStops(secureStopRelease);
    } else {
        status = mPlugin->releaseSecureStop(toHidlVec(ssRelease));
    }
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::removeSecureStop(Vector<uint8_t> const& ssid) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPluginV1_1 == NULL) {
        return ERROR_DRM_CANNOT_HANDLE;
    }

    Return<Status> status = mPluginV1_1->removeSecureStop(toHidlVec(ssid));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::removeAllSecureStops() {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status(Status::ERROR_DRM_UNKNOWN);
    if (mPluginV1_1 != NULL) {
        status = mPluginV1_1->removeAllSecureStops();
    } else {
        status = mPlugin->releaseAllSecureStops();
    }
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getHdcpLevels(DrmPlugin::HdcpLevel* connected,
                                    DrmPlugin::HdcpLevel* max) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    if (connected == NULL || max == NULL) {
        return BAD_VALUE;
    }
    DrmStatus err = UNKNOWN_ERROR;

    *connected = DrmPlugin::kHdcpLevelUnknown;
    *max = DrmPlugin::kHdcpLevelUnknown;

    Return<void> hResult;
    if (mPluginV1_2 != NULL) {
        hResult = mPluginV1_2->getHdcpLevels_1_2([&](Status_V1_2 status,
                                                     const HdcpLevel_V1_2& hConnected,
                                                     const HdcpLevel_V1_2& hMax) {
            if (status == Status_V1_2::OK) {
                *connected = toHdcpLevel(hConnected);
                *max = toHdcpLevel(hMax);
            }
            err = toStatusT(status);
        });
    } else if (mPluginV1_1 != NULL) {
        hResult = mPluginV1_1->getHdcpLevels(
                [&](Status status, const HdcpLevel& hConnected, const HdcpLevel& hMax) {
                    if (status == Status::OK) {
                        *connected = toHdcpLevel(static_cast<HdcpLevel_V1_2>(hConnected));
                        *max = toHdcpLevel(static_cast<HdcpLevel_V1_2>(hMax));
                    }
                    err = toStatusT(status);
                });
    } else {
        return DrmStatus(ERROR_DRM_CANNOT_HANDLE);
    }

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getNumberOfSessions(uint32_t* open, uint32_t* max) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    if (open == NULL || max == NULL) {
        return BAD_VALUE;
    }
    DrmStatus err = UNKNOWN_ERROR;

    *open = 0;
    *max = 0;

    if (mPluginV1_1 == NULL) {
        return DrmStatus(ERROR_DRM_CANNOT_HANDLE);
    }

    Return<void> hResult =
            mPluginV1_1->getNumberOfSessions([&](Status status, uint32_t hOpen, uint32_t hMax) {
                if (status == Status::OK) {
                    *open = hOpen;
                    *max = hMax;
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getSecurityLevel(Vector<uint8_t> const& sessionId,
                                       DrmPlugin::SecurityLevel* level) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    if (level == NULL) {
        return DrmStatus(BAD_VALUE);
    }
    DrmStatus err = UNKNOWN_ERROR;

    if (mPluginV1_1 == NULL) {
        return DrmStatus(ERROR_DRM_CANNOT_HANDLE);
    }

    *level = DrmPlugin::kSecurityLevelUnknown;

    Return<void> hResult = mPluginV1_1->getSecurityLevel(toHidlVec(sessionId),
                                                         [&](Status status, SecurityLevel hLevel) {
                                                             if (status == Status::OK) {
                                                                 *level = toSecurityLevel(hLevel);
                                                             }
                                                             err = toStatusT(status);
                                                         });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getOfflineLicenseKeySetIds(List<Vector<uint8_t>>& keySetIds) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return DrmStatus(mInitCheck);
    }

    if (mPluginV1_2 == NULL) {
        return DrmStatus(ERROR_UNSUPPORTED);
    }

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPluginV1_2->getOfflineLicenseKeySetIds(
            [&](Status status, const hidl_vec<KeySetId>& hKeySetIds) {
                if (status == Status::OK) {
                    keySetIds = toKeySetIds(hKeySetIds);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::removeOfflineLicense(Vector<uint8_t> const& keySetId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return DrmStatus(mInitCheck);
    }

    if (mPluginV1_2 == NULL) {
        return DrmStatus(ERROR_UNSUPPORTED);
    }

    Return<Status> status = mPluginV1_2->removeOfflineLicense(toHidlVec(keySetId));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getOfflineLicenseState(Vector<uint8_t> const& keySetId,
                                             DrmPlugin::OfflineLicenseState* licenseState) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return DrmStatus(mInitCheck);
    }

    if (mPluginV1_2 == NULL) {
        return DrmStatus(ERROR_UNSUPPORTED);
    }
    *licenseState = DrmPlugin::kOfflineLicenseStateUnknown;

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPluginV1_2->getOfflineLicenseState(
            toHidlVec(keySetId), [&](Status status, OfflineLicenseState hLicenseState) {
                if (status == Status::OK) {
                    *licenseState = toOfflineLicenseState(hLicenseState);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getPropertyString(String8 const& name, String8& value) const {
    Mutex::Autolock autoLock(mLock);
    return getPropertyStringInternal(name, value);
}

DrmStatus DrmHalHidl::getPropertyStringInternal(String8 const& name, String8& value) const {
    // This function is internal to the class and should only be called while
    // mLock is already held.
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->getPropertyString(
            toHidlString(name), [&](Status status, const hidl_string& hValue) {
                if (status == Status::OK) {
                    value = toString8(hValue);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getPropertyByteArray(String8 const& name, Vector<uint8_t>& value) const {
    Mutex::Autolock autoLock(mLock);
    return getPropertyByteArrayInternal(name, value);
}

DrmStatus DrmHalHidl::getPropertyByteArrayInternal(String8 const& name,
                                                   Vector<uint8_t>& value) const {
    // This function is internal to the class and should only be called while
    // mLock is already held.
    INIT_CHECK();

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->getPropertyByteArray(
            toHidlString(name), [&](Status status, const hidl_vec<uint8_t>& hValue) {
                if (status == Status::OK) {
                    value = toVector(hValue);
                }
                err = toStatusT(status);
            });

    err = hResult.isOk() ? err : DEAD_OBJECT;
    if (name == kPropertyDeviceUniqueId) {
        mMetrics.mGetDeviceUniqueIdCounter.Increment(err);
    }
    return err;
}

DrmStatus DrmHalHidl::setPropertyString(String8 const& name, String8 const& value) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status = mPlugin->setPropertyString(toHidlString(name), toHidlString(value));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::setPropertyByteArray(String8 const& name,
                                           Vector<uint8_t> const& value) const {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    Return<Status> status = mPlugin->setPropertyByteArray(toHidlString(name), toHidlVec(value));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getMetrics(const sp<IDrmMetricsConsumer>& consumer) {
    if (consumer == nullptr) {
        return DrmStatus(UNEXPECTED_NULL);
    }
    consumer->consumeFrameworkMetrics(mMetrics);

    // Append vendor metrics if they are supported.
    if (mPluginV1_1 != NULL) {
        String8 vendor;
        String8 description;
        if (getPropertyStringInternal(String8("vendor"), vendor) != OK || vendor.empty()) {
            ALOGE("Get vendor failed or is empty");
            vendor = "NONE";
        }
        if (getPropertyStringInternal(String8("description"), description) != OK ||
            description.empty()) {
            ALOGE("Get description failed or is empty.");
            description = "NONE";
        }
        vendor += ".";
        vendor += description;

        hidl_vec<DrmMetricGroup> pluginMetrics;
        DrmStatus err = UNKNOWN_ERROR;

        Return<void> status =
                mPluginV1_1->getMetrics([&](Status status, hidl_vec<DrmMetricGroup> pluginMetrics) {
                    if (status != Status::OK) {
                        ALOGV("Error getting plugin metrics: %d", status);
                    } else {
                        consumer->consumeHidlMetrics(vendor, pluginMetrics);
                    }
                    err = toStatusT(status);
                });
        return status.isOk() ? err : DrmStatus(DEAD_OBJECT);
    }

    return DrmStatus(OK);
}

DrmStatus DrmHalHidl::setCipherAlgorithm(Vector<uint8_t> const& sessionId,
                                         String8 const& algorithm) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    Return<Status> status =
            mPlugin->setCipherAlgorithm(toHidlVec(sessionId), toHidlString(algorithm));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::setMacAlgorithm(Vector<uint8_t> const& sessionId, String8 const& algorithm) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    Return<Status> status = mPlugin->setMacAlgorithm(toHidlVec(sessionId), toHidlString(algorithm));
    return status.isOk() ? DrmStatus(toStatusT(status)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::encrypt(Vector<uint8_t> const& sessionId, Vector<uint8_t> const& keyId,
                              Vector<uint8_t> const& input, Vector<uint8_t> const& iv,
                              Vector<uint8_t>& output) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult =
            mPlugin->encrypt(toHidlVec(sessionId), toHidlVec(keyId), toHidlVec(input),
                             toHidlVec(iv), [&](Status status, const hidl_vec<uint8_t>& hOutput) {
                                 if (status == Status::OK) {
                                     output = toVector(hOutput);
                                 }
                                 err = toStatusT(status);
                             });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::decrypt(Vector<uint8_t> const& sessionId, Vector<uint8_t> const& keyId,
                              Vector<uint8_t> const& input, Vector<uint8_t> const& iv,
                              Vector<uint8_t>& output) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult =
            mPlugin->decrypt(toHidlVec(sessionId), toHidlVec(keyId), toHidlVec(input),
                             toHidlVec(iv), [&](Status status, const hidl_vec<uint8_t>& hOutput) {
                                 if (status == Status::OK) {
                                     output = toVector(hOutput);
                                 }
                                 err = toStatusT(status);
                             });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::sign(Vector<uint8_t> const& sessionId, Vector<uint8_t> const& keyId,
                           Vector<uint8_t> const& message, Vector<uint8_t>& signature) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->sign(toHidlVec(sessionId), toHidlVec(keyId), toHidlVec(message),
                                         [&](Status status, const hidl_vec<uint8_t>& hSignature) {
                                             if (status == Status::OK) {
                                                 signature = toVector(hSignature);
                                             }
                                             err = toStatusT(status);
                                         });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::verify(Vector<uint8_t> const& sessionId, Vector<uint8_t> const& keyId,
                             Vector<uint8_t> const& message, Vector<uint8_t> const& signature,
                             bool& match) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult =
            mPlugin->verify(toHidlVec(sessionId), toHidlVec(keyId), toHidlVec(message),
                            toHidlVec(signature), [&](Status status, bool hMatch) {
                                if (status == Status::OK) {
                                    match = hMatch;
                                } else {
                                    match = false;
                                }
                                err = toStatusT(status);
                            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::signRSA(Vector<uint8_t> const& sessionId, String8 const& algorithm,
                              Vector<uint8_t> const& message, Vector<uint8_t> const& wrappedKey,
                              Vector<uint8_t>& signature) {
    Mutex::Autolock autoLock(mLock);
    INIT_CHECK();

    DrmSessionManager::Instance()->useSession(sessionId);

    DrmStatus err = UNKNOWN_ERROR;

    Return<void> hResult = mPlugin->signRSA(
            toHidlVec(sessionId), toHidlString(algorithm), toHidlVec(message),
            toHidlVec(wrappedKey), [&](Status status, const hidl_vec<uint8_t>& hSignature) {
                if (status == Status::OK) {
                    signature = toVector(hSignature);
                }
                err = toStatusT(status);
            });

    return hResult.isOk() ? err : DrmStatus(DEAD_OBJECT);
}

std::string DrmHalHidl::reportFrameworkMetrics(const std::string& pluginMetrics) const {
    mediametrics_handle_t item(mediametrics_create("mediadrm"));
    mediametrics_setUid(item, mMetrics.GetAppUid());
    String8 vendor;
    String8 description;
    status_t result = getPropertyStringInternal(String8("vendor"), vendor);
    if (result != OK) {
        ALOGE("Failed to get vendor from drm plugin: %d", result);
    } else {
        mediametrics_setCString(item, "vendor", vendor.c_str());
    }
    result = getPropertyStringInternal(String8("description"), description);
    if (result != OK) {
        ALOGE("Failed to get description from drm plugin: %d", result);
    } else {
        mediametrics_setCString(item, "description", description.c_str());
    }

    std::string serializedMetrics;
    result = mMetrics.GetSerializedMetrics(&serializedMetrics);
    if (result != OK) {
        ALOGE("Failed to serialize framework metrics: %d", result);
    }
    std::string b64EncodedMetrics =
            toBase64StringNoPad(serializedMetrics.data(), serializedMetrics.size());
    if (!b64EncodedMetrics.empty()) {
        mediametrics_setCString(item, "serialized_metrics", b64EncodedMetrics.c_str());
    }
    if (!pluginMetrics.empty()) {
        mediametrics_setCString(item, "plugin_metrics", pluginMetrics.c_str());
    }
    if (!mediametrics_selfRecord(item)) {
        ALOGE("Failed to self record framework metrics");
    }
    mediametrics_delete(item);
    return serializedMetrics;
}

std::string DrmHalHidl::reportPluginMetrics() const {
    Vector<uint8_t> metricsVector;
    String8 vendor;
    String8 description;
    std::string metricsString;
    if (getPropertyStringInternal(String8("vendor"), vendor) == OK &&
        getPropertyStringInternal(String8("description"), description) == OK &&
        getPropertyByteArrayInternal(String8("metrics"), metricsVector) == OK) {
        metricsString = toBase64StringNoPad(metricsVector.array(), metricsVector.size());
        status_t res = android::reportDrmPluginMetrics(metricsString, vendor, description,
                                                       mMetrics.GetAppUid());
        if (res != OK) {
            ALOGE("Metrics were retrieved but could not be reported: %d", res);
        }
    }
    return metricsString;
}

DrmStatus DrmHalHidl::requiresSecureDecoder(const char* mime, bool* required) const {
    Mutex::Autolock autoLock(mLock);
    if (mPluginV1_4 == NULL) {
        return DrmStatus(false);
    }
    auto hResult = mPluginV1_4->requiresSecureDecoderDefault(hidl_string(mime));
    if (!hResult.isOk()) {
        DrmUtils::LOG2BE("requiresSecureDecoder txn failed: %s", hResult.description().c_str());
        return DrmStatus(DEAD_OBJECT);
    }
    if (required) {
        *required = hResult;
    }
    return DrmStatus(OK);
}

DrmStatus DrmHalHidl::requiresSecureDecoder(const char* mime,
                                            DrmPlugin::SecurityLevel securityLevel,
                                            bool* required) const {
    Mutex::Autolock autoLock(mLock);
    if (mPluginV1_4 == NULL) {
        return DrmStatus(false);
    }
    auto hLevel = toHidlSecurityLevel(securityLevel);
    auto hResult = mPluginV1_4->requiresSecureDecoder(hidl_string(mime), hLevel);
    if (!hResult.isOk()) {
        DrmUtils::LOG2BE("requiresSecureDecoder txn failed: %s", hResult.description().c_str());
        return DrmStatus(DEAD_OBJECT);
    }
    if (required) {
        *required = hResult;
    }
    return DrmStatus(OK);
}

DrmStatus DrmHalHidl::setPlaybackId(Vector<uint8_t> const& sessionId, const char* playbackId) {
    Mutex::Autolock autoLock(mLock);
    if (mPluginV1_4 == NULL) {
        return DrmStatus(ERROR_UNSUPPORTED);
    }
    auto err = mPluginV1_4->setPlaybackId(toHidlVec(sessionId), hidl_string(playbackId));
    return err.isOk() ? DrmStatus(toStatusT(err)) : DrmStatus(DEAD_OBJECT);
}

DrmStatus DrmHalHidl::getLogMessages(Vector<drm::V1_4::LogMessage>& logs) const {
    Mutex::Autolock autoLock(mLock);
    return DrmStatus(DrmUtils::GetLogMessages<drm::V1_4::IDrmPlugin>(mPlugin, logs));
}

DrmStatus DrmHalHidl::getSupportedSchemes(std::vector<uint8_t>& schemes) const {
    Mutex::Autolock autoLock(mLock);
    for (auto &factory : mFactories) {
        sp<drm::V1_3::IDrmFactory> factoryV1_3 = drm::V1_3::IDrmFactory::castFrom(factory);
        if (factoryV1_3 == nullptr) {
            continue;
        }

        factoryV1_3->getSupportedCryptoSchemes(
            [&](const hardware::hidl_vec<hardware::hidl_array<uint8_t, 16>>& schemes_hidl) {
                for (const auto &scheme : schemes_hidl) {
                    schemes.insert(schemes.end(), scheme.data(), scheme.data() + scheme.size());
                }
            });
    }

    return DrmStatus(OK);
}

}  // namespace android
