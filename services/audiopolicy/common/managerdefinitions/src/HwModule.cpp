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

#define LOG_TAG "APM::HwModule"
//#define LOG_NDEBUG 0

#include <android-base/stringprintf.h>
#include <policy.h>
#include <system/audio.h>

#include "HwModule.h"
#include "IOProfile.h"

namespace android {

HwModule::HwModule(const char *name, uint32_t halVersionMajor, uint32_t halVersionMinor)
    : mName(String8(name)),
      mHandle(AUDIO_MODULE_HANDLE_NONE)
{
    setHalVersion(halVersionMajor, halVersionMinor);
}

HwModule::HwModule(const char *name, uint32_t halVersion)
    : mName(String8(name)),
      mHandle(AUDIO_MODULE_HANDLE_NONE),
      mHalVersion(halVersion)
{
}

HwModule::~HwModule()
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        mOutputProfiles[i]->clearSupportedDevices();
    }
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        mInputProfiles[i]->clearSupportedDevices();
    }
}

std::string HwModule::getTagForDevice(audio_devices_t device, const String8 &address,
                                          audio_format_t codec)
{
    DeviceVector declaredDevices = getDeclaredDevices();
    sp<DeviceDescriptor> deviceDesc = declaredDevices.getDevice(device, address, codec);
    return deviceDesc ? deviceDesc->getTagName() : std::string{};
}

status_t HwModule::addOutputProfile(const std::string& name, const audio_config_t *config,
                                    audio_devices_t device, const String8& address,
                                    audio_output_flags_t flags)
{
    sp<IOProfile> profile = new OutputProfile(name);
    profile->addAudioProfile(new AudioProfile(config->format, config->channel_mask,
                                              config->sample_rate));
    profile->setFlags(flags);

    sp<DeviceDescriptor> devDesc =
            new DeviceDescriptor(device, getTagForDevice(device), address.c_str());
    addDynamicDevice(devDesc);
    // Reciprocally attach the device to the module
    devDesc->attach(this);
    profile->addSupportedDevice(devDesc);

    return addOutputProfile(profile);
}

status_t HwModule::addOutputProfile(const sp<IOProfile> &profile)
{
    profile->attach(this);
    mOutputProfiles.add(profile);
    mPorts.add(profile);
    return NO_ERROR;
}

status_t HwModule::addInputProfile(const sp<IOProfile> &profile)
{
    profile->attach(this);
    mInputProfiles.add(profile);
    mPorts.add(profile);
    return NO_ERROR;
}

status_t HwModule::addProfile(const sp<IOProfile> &profile)
{
    switch (profile->getRole()) {
    case AUDIO_PORT_ROLE_SOURCE:
        return addOutputProfile(profile);
    case AUDIO_PORT_ROLE_SINK:
        return addInputProfile(profile);
    case AUDIO_PORT_ROLE_NONE:
        return BAD_VALUE;
    }
    return BAD_VALUE;
}

void HwModule::setProfiles(const IOProfileCollection &profiles)
{
    for (size_t i = 0; i < profiles.size(); i++) {
        addProfile(profiles[i]);
    }
}

status_t HwModule::removeOutputProfile(const std::string& name)
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        if (mOutputProfiles[i]->getName() == name) {
            for (const auto &device : mOutputProfiles[i]->getSupportedDevices()) {
                removeDynamicDevice(device);
            }
            mOutputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

status_t HwModule::addInputProfile(const std::string& name, const audio_config_t *config,
                                   audio_devices_t device, const String8& address,
                                   audio_input_flags_t flags)
{
    sp<IOProfile> profile = new InputProfile(name);
    profile->addAudioProfile(new AudioProfile(config->format, config->channel_mask,
                                              config->sample_rate));
    profile->setFlags(flags);

    sp<DeviceDescriptor> devDesc =
            new DeviceDescriptor(device, getTagForDevice(device), address.c_str());
    addDynamicDevice(devDesc);
    // Reciprocally attach the device to the module
    devDesc->attach(this);
    profile->addSupportedDevice(devDesc);

    ALOGV("addInputProfile() name %s rate %d mask 0x%08x",
          name.c_str(), config->sample_rate, config->channel_mask);

    return addInputProfile(profile);
}

status_t HwModule::removeInputProfile(const std::string& name)
{
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        if (mInputProfiles[i]->getName() == name) {
            for (const auto &device : mInputProfiles[i]->getSupportedDevices()) {
                removeDynamicDevice(device);
            }
            mInputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

void HwModule::setDeclaredDevices(const DeviceVector &devices)
{
    mDeclaredDevices = devices;
    for (size_t i = 0; i < devices.size(); i++) {
        mPorts.add(devices[i]);
    }
}

sp<DeviceDescriptor> HwModule::getRouteSinkDevice(const sp<AudioRoute> &route) const
{
    sp<DeviceDescriptor> sinkDevice = 0;
    if (route->getSink()->asAudioPort()->getType() == AUDIO_PORT_TYPE_DEVICE) {
        sinkDevice = mDeclaredDevices.getDeviceFromTagName(route->getSink()->getTagName());
    }
    return sinkDevice;
}

DeviceVector HwModule::getRouteSourceDevices(const sp<AudioRoute> &route) const
{
    DeviceVector sourceDevices;
    for (const auto& source : route->getSources()) {
        if (source->asAudioPort()->getType() == AUDIO_PORT_TYPE_DEVICE) {
            sourceDevices.add(mDeclaredDevices.getDeviceFromTagName(source->getTagName()));
        }
    }
    return sourceDevices;
}

void HwModule::setRoutes(const AudioRouteVector &routes)
{
    mRoutes = routes;
    // Now updating the streams (aka IOProfile until now) supported devices
    refreshSupportedDevices();
}

void HwModule::refreshSupportedDevices()
{
    // Now updating the streams (aka IOProfile until now) supported devices
    for (const auto& stream : mInputProfiles) {
        DeviceVector sourceDevices;
        for (const auto& route : stream->getRoutes()) {
            sp<PolicyAudioPort> sink = route->getSink();
            if (sink == 0 || stream != sink) {
                ALOGE("%s: Invalid route attached to input stream", __FUNCTION__);
                continue;
            }
            DeviceVector sourceDevicesForRoute = getRouteSourceDevices(route);
            if (sourceDevicesForRoute.isEmpty()) {
                ALOGE("%s: invalid source devices for %s", __FUNCTION__, stream->getName().c_str());
                continue;
            }
            sourceDevices.add(sourceDevicesForRoute);
        }
        if (sourceDevices.isEmpty()) {
            ALOGE("%s: invalid source devices for %s", __FUNCTION__, stream->getName().c_str());
            continue;
        }
        stream->setSupportedDevices(sourceDevices);
    }
    for (const auto& stream : mOutputProfiles) {
        DeviceVector sinkDevices;
        for (const auto& route : stream->getRoutes()) {
            sp<PolicyAudioPort> source = findByTagName(route->getSources(), stream->getTagName());
            if (source == 0 || stream != source) {
                ALOGE("%s: Invalid route attached to output stream", __FUNCTION__);
                continue;
            }
            sp<DeviceDescriptor> sinkDevice = getRouteSinkDevice(route);
            if (sinkDevice == 0) {
                ALOGE("%s: invalid sink device for %s", __FUNCTION__, stream->getName().c_str());
                continue;
            }
            sinkDevices.add(sinkDevice);
        }
        stream->setSupportedDevices(sinkDevices);
    }
}

void HwModule::setHandle(audio_module_handle_t handle) {
    ALOGW_IF(mHandle != AUDIO_MODULE_HANDLE_NONE,
            "HwModule handle is changing from %d to %d", mHandle, handle);
    mHandle = handle;
}

bool HwModule::supportsPatch(const sp<PolicyAudioPort> &srcPort,
                             const sp<PolicyAudioPort> &dstPort) const {
    for (const auto &route : mRoutes) {
        if (route->supportsPatch(srcPort, dstPort)) {
            return true;
        }
    }
    return false;
}

void HwModule::dump(String8 *dst, int spaces) const
{
    dst->appendFormat("Handle: %d; \"%s\"\n", mHandle, getName());
    if (mOutputProfiles.size()) {
        dst->appendFormat("%*s- Output MixPorts (%zu):\n", spaces - 2, "", mOutputProfiles.size());
        for (size_t i = 0; i < mOutputProfiles.size(); i++) {
            const std::string prefix = base::StringPrintf("%*s %zu. ", spaces, "", i + 1);
            dst->append(prefix.c_str());
            mOutputProfiles[i]->dump(dst, prefix.size());
        }
    }
    if (mInputProfiles.size()) {
        dst->appendFormat("%*s- Input MixPorts (%zu):\n", spaces - 2, "", mInputProfiles.size());
        for (size_t i = 0; i < mInputProfiles.size(); i++) {
            const std::string prefix = base::StringPrintf("%*s %zu. ", spaces, "", i + 1);
            dst->append(prefix.c_str());
            mInputProfiles[i]->dump(dst, prefix.size());
        }
    }
    mDeclaredDevices.dump(dst, String8("- Declared"), spaces - 2, true);
    mDynamicDevices.dump(dst, String8("- Dynamic"),  spaces - 2, true);
    dumpAudioRouteVector(mRoutes, dst, spaces);
}

sp<HwModule> HwModuleCollection::getModuleFromHandle(audio_module_handle_t handle) const
{
    for (const auto& module : *this) {
        if (module->getHandle() == handle) {
            return module;
        }
    }
    return nullptr;
}

sp <HwModule> HwModuleCollection::getModuleFromName(const char *name) const
{
    for (const auto& module : *this) {
        if (strcmp(module->getName(), name) == 0) {
            return module;
        }
    }
    return nullptr;
}

sp<HwModule> HwModuleCollection::getModuleForDeviceType(audio_devices_t type,
                                                        audio_format_t encodedFormat,
                                                        std::string *tagName) const
{
    for (const auto& module : *this) {
        const auto& profiles = audio_is_output_device(type) ?
                module->getOutputProfiles() : module->getInputProfiles();
        for (const auto& profile : profiles) {
            if (profile->supportsDeviceTypes({type})) {
                if (encodedFormat != AUDIO_FORMAT_DEFAULT) {
                    DeviceVector declaredDevices = module->getDeclaredDevices();
                    sp <DeviceDescriptor> deviceDesc =
                            declaredDevices.getDevice(type, String8(), encodedFormat);
                    if (deviceDesc) {
                        if (tagName != nullptr) {
                            *tagName = deviceDesc->getTagName();
                        }
                        return module;
                    }
                } else {
                    if (tagName != nullptr) {
                        *tagName = profile->getTag({type});
                    }
                    return module;
                }
            }
        }
    }
    return nullptr;
}

sp<HwModule> HwModuleCollection::getModuleForDevice(const sp<DeviceDescriptor> &device,
                                                     audio_format_t encodedFormat) const
{
    return getModuleForDeviceType(device->type(), encodedFormat);
}

DeviceVector HwModuleCollection::getAvailableDevicesFromModuleName(
        const char *name, const DeviceVector &availableDevices) const
{
    sp<HwModule> module = getModuleFromName(name);
    if (module == nullptr) {
        return DeviceVector();
    }
    return availableDevices.getDevicesFromHwModule(module->getHandle());
}

sp<DeviceDescriptor> HwModuleCollection::getDeviceDescriptor(const audio_devices_t deviceType,
                                                             const char *address,
                                                             const char *name,
                                                             const audio_format_t encodedFormat,
                                                             bool allowToCreate,
                                                             bool matchAddress) const
{
    String8 devAddress = (address == nullptr || !matchAddress) ? String8("") : String8(address);
    // handle legacy remote submix case where the address was not always specified
    if (audio_is_remote_submix_device(deviceType) && (devAddress.length() == 0)) {
        devAddress = String8("0");
    }

    for (const auto& hwModule : *this) {
        if (!allowToCreate) {
            auto dynamicDevices = hwModule->getDynamicDevices();
            auto dynamicDevice = dynamicDevices.getDevice(deviceType, devAddress, encodedFormat);
            if (dynamicDevice) {
                return dynamicDevice;
            }
        }
        DeviceVector moduleDevices = hwModule->getAllDevices();
        auto moduleDevice = moduleDevices.getDevice(deviceType, devAddress, encodedFormat);

        // Prevent overwriting moduleDevice address if connected device does not have the same
        // address (since getDevice with empty address ignores match on address), use dynamic device
        if (moduleDevice && allowToCreate &&
                (!moduleDevice->address().empty() &&
                 (moduleDevice->address().compare(devAddress.c_str()) != 0))) {
            break;
        }
        if (moduleDevice) {
            if (encodedFormat != AUDIO_FORMAT_DEFAULT) {
                moduleDevice->setEncodedFormat(encodedFormat);
            }
            if (allowToCreate) {
                moduleDevice->attach(hwModule);
                // Name may be overwritten, restored on detach.
                moduleDevice->setAddress(devAddress.c_str());
                // Name may be overwritten, restored on detach.
                moduleDevice->setName(name);
            }
            return moduleDevice;
        }
    }
    if (!allowToCreate) {
        ALOGW("%s: could not find HW module for device %s (%s, %08x) address %s", __FUNCTION__,
                name, audio_device_to_string(deviceType), deviceType, address);
        return nullptr;
    }
    return createDevice(deviceType, address, name, encodedFormat);
}

sp<DeviceDescriptor> HwModuleCollection::createDevice(const audio_devices_t type,
                                                      const char *address,
                                                      const char *name,
                                                      const audio_format_t encodedFormat) const
{
    std::string tagName = {};
    sp<HwModule> hwModule = getModuleForDeviceType(type, encodedFormat, &tagName);
    if (hwModule == 0) {
        if (encodedFormat == AUDIO_FORMAT_DEFAULT) {
            ALOGE("%s: could not find HW module for device type '%s' (%08x)",
                    __FUNCTION__, audio_device_to_string(type), type);
        } else {
            ALOGE("%s: could not find HW module for device type '%s' (%08x), "
                    "encoded format '%s'", __FUNCTION__, audio_device_to_string(type), type,
                    audio_format_to_string(encodedFormat));
        }
        return nullptr;
    }

    sp<DeviceDescriptor> device = new DeviceDescriptor(type, tagName, address);
    device->setName(name);
    device->setEncodedFormat(encodedFormat);
    device->setDynamic();
    // Add the device to the list of dynamic devices
    hwModule->addDynamicDevice(device);
    // Reciprocally attach the device to the module
    device->attach(hwModule);
    ALOGD("%s: adding dynamic device %s to module %s", __FUNCTION__,
          device->toString().c_str(), hwModule->getName());

    const auto &profiles = (audio_is_output_device(type) ? hwModule->getOutputProfiles() :
                                                             hwModule->getInputProfiles());
    for (const auto &profile : profiles) {
        // Add the device as supported to all profile supporting "weakly" or not the device
        // according to its type
        if (profile->supportsDevice(device, false /*matchAddress*/)) {

            // @todo quid of audio profile? import the profile from device of the same type?
            const auto &isoTypeDeviceForProfile =
                profile->getSupportedDevices().getDevice(type, String8(), AUDIO_FORMAT_DEFAULT);
            device->importAudioPortAndPickAudioProfile(isoTypeDeviceForProfile, true /* force */);

            ALOGV("%s: adding device %s to profile %s", __FUNCTION__,
                  device->toString().c_str(), profile->getTagName().c_str());
            profile->addSupportedDevice(device);
        }
    }
    return device;
}

void HwModuleCollection::cleanUpForDevice(const sp<DeviceDescriptor> &device)
{
    for (const auto& hwModule : *this) {
        DeviceVector moduleDevices = hwModule->getAllDevices();
        if (!moduleDevices.contains(device)) {
            continue;
        }

        // removal of remote submix devices associated with a dynamic policy is
        // handled by removeOutputProfile() and removeInputProfile()
        if (audio_is_remote_submix_device(device->type()) && device->address() != "0") {
            continue;
        }

        device->detach();
        // Only remove from dynamic list, not from declared list!!!
        if (!hwModule->removeDynamicDevice(device)) {
            return;
        }
        ALOGV("%s: removed dynamic device %s from module %s", __FUNCTION__,
              device->toString().c_str(), hwModule->getName());

        const IOProfileCollection &profiles = audio_is_output_device(device->type()) ?
                    hwModule->getOutputProfiles() : hwModule->getInputProfiles();
        for (const auto &profile : profiles) {
            // For cleanup, strong match is required
            if (profile->supportsDevice(device, true /*matchAdress*/)) {
                ALOGV("%s: removing device %s from profile %s", __FUNCTION__,
                      device->toString().c_str(), profile->getTagName().c_str());
                profile->removeSupportedDevice(device);
            }
        }
    }
}

void HwModuleCollection::dump(String8 *dst) const
{
    dst->appendFormat("\n Hardware modules (%zu):\n", size());
    for (size_t i = 0; i < size(); i++) {
        const std::string prefix = base::StringPrintf("  %zu. ", i + 1);
        dst->append(prefix.c_str());
        itemAt(i)->dump(dst, prefix.size());
    }
}


} //namespace android
