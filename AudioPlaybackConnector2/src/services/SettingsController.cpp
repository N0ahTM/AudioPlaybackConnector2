#include <pch.h>

#include <services/SettingsController.hpp>

#include <core/DeviceManager.hpp>
#include <core/SettingsLimits.hpp>
#include <core/StringResources.hpp>

namespace {

[[nodiscard]] bool IsValidDeviceId(std::wstring_view value) noexcept {
    return !value.empty() && apc::limits::IsBoundedUtf16(value, apc::limits::c_maxDeviceIdCharacters);
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsController::SettingsController(std::shared_ptr<Settings> settings,
                                       std::weak_ptr<DeviceManager> deviceManager,
                                       std::function<void()> requestSave)
    : m_settings(std::move(settings)), m_deviceManager(std::move(deviceManager)),
      m_requestSave(std::move(requestSave)) {}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsData SettingsController::Snapshot() const {
    if (!m_settings) return {};
    auto locked = m_settings->LockSharedData();
    return *locked;
}

void SettingsController::SetPresentationChangedCallback(PresentationChangedCallback callback) {
    m_presentationChanged = std::move(callback);
}

void SettingsController::SetGlobalConnectOnStartup(bool enabled) {
    if (!m_settings) return;

    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->GlobalConnectOnStartup == enabled) return;
        locked.Mutate().GlobalConnectOnStartup = enabled;
    }

    RequestSave();
}

void SettingsController::SetGlobalReconnectOnConnectionLoss(bool enabled) {
    if (!m_settings) return;

    std::vector<DeviceSettings> devices;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->GlobalReconnectOnConnectionLoss == enabled) return;
        locked.Mutate().GlobalReconnectOnConnectionLoss = enabled;
        devices = locked->Devices;
    }

    if (auto manager = m_deviceManager.lock()) {
        std::vector<std::wstring> individuallyEnabledDeviceIds;
        individuallyEnabledDeviceIds.reserve(devices.size());
        for (auto const& device : devices) {
            if (device.ReconnectOnConnectionLoss) individuallyEnabledDeviceIds.push_back(device.Id);
        }
        manager->ApplyReconnectOnConnectionLossPolicy(enabled, individuallyEnabledDeviceIds);
    }

    RequestSave();
}

void SettingsController::SetAllowIncomingConnections(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->AllowIncomingConnections == enabled) return;
        locked.Mutate().AllowIncomingConnections = enabled;
    }

    if (auto manager = m_deviceManager.lock()) {
        manager->SetIncomingConnectionsEnabled(enabled);
    }

    RequestSave();
}

void SettingsController::SetStartWithWindows(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->StartWithWindows == enabled) return;
        locked.Mutate().StartWithWindows = enabled;
    }
    RequestSave();
}

void SettingsController::SetShowNotifications(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->ShowNotifications == enabled) return;
        locked.Mutate().ShowNotifications = enabled;
    }
    RequestSave();
}

void SettingsController::SetSystemBackdropEffects(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->UseSystemBackdropEffects == enabled) return;
        locked.Mutate().UseSystemBackdropEffects = enabled;
    }
    RequestSave();
    NotifyPresentationChanged(PresentationChangeKind::Appearance);
}

void SettingsController::SetLanguage(std::wstring language) {
    if (!m_settings) return;
    if (language.empty()) {
        language = L"system";
    } else if (!apc::limits::IsSupportedLanguage(language)) {
        return;
    }
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->Language == language) return;
        locked.Mutate().Language = language;
    }
    RequestSave();
    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), language);
    NotifyPresentationChanged(PresentationChangeKind::Language);
}

void SettingsController::SetPrivacyMode(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->PrivacyModeEnabled == enabled) return;
        locked.Mutate().PrivacyModeEnabled = enabled;
    }
    RequestSave();
    NotifyPresentationChanged();
}

bool SettingsController::SetSettingsWindowBounds(PersistedWindowBounds bounds) {
    if (!m_settings || bounds.Width <= 0 || bounds.Height <= 0 || bounds.Dpi < apc::limits::c_minWindowDpi ||
        bounds.Dpi > apc::limits::c_maxWindowDpi) {
        return false;
    }

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->SettingsWindowBounds != bounds) {
            locked.Mutate().SettingsWindowBounds = bounds;
            changed = true;
        }
    }

    return changed;
}

bool SettingsController::ClearSettingsWindowBounds() {
    if (!m_settings) return false;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->SettingsWindowBounds) {
            locked.Mutate().SettingsWindowBounds.reset();
            changed = true;
        }
    }

    return changed;
}

void SettingsController::Save() {
    RequestSave();
}

void SettingsController::SetDeviceConnectOnStartup(std::wstring const& deviceId, bool enabled) {
    if (!m_settings || !IsValidDeviceId(deviceId)) return;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        auto const device = std::ranges::find(locked->Devices, deviceId, &DeviceSettings::Id);
        if (device == locked->Devices.end()) return;
        if (device->ConnectOnStartup == enabled) return;
        auto const index = static_cast<std::size_t>(std::distance(locked->Devices.begin(), device));
        locked.Mutate().Devices[index].ConnectOnStartup = enabled;
        changed = true;
    }
    if (!changed) return;

    RequestSave();
}

void SettingsController::SetDeviceReconnectOnConnectionLoss(std::wstring const& deviceId, bool enabled) {
    if (!m_settings || !IsValidDeviceId(deviceId)) return;

    bool changed = false;
    bool globalReconnectOnConnectionLoss = false;
    {
        auto locked = m_settings->LockExclusiveData();
        auto const device = std::ranges::find(locked->Devices, deviceId, &DeviceSettings::Id);
        if (device == locked->Devices.end()) return;
        if (device->ReconnectOnConnectionLoss == enabled) return;
        auto const index = static_cast<std::size_t>(std::distance(locked->Devices.begin(), device));
        locked.Mutate().Devices[index].ReconnectOnConnectionLoss = enabled;
        changed = true;
        globalReconnectOnConnectionLoss = locked->GlobalReconnectOnConnectionLoss;
    }
    if (!changed) return;

    if (auto manager = m_deviceManager.lock()) {
        manager->SetReconnectOnConnectionLoss(winrt::hstring(deviceId), globalReconnectOnConnectionLoss || enabled);
    }

    RequestSave();
}

bool SettingsController::SetDeviceAlias(std::wstring const& deviceId,
                                        std::wstring alias,
                                        std::wstring const& deviceName) {
    if (!m_settings || !IsValidDeviceId(deviceId) ||
        !apc::limits::IsBoundedUtf16(alias, apc::limits::c_maxDeviceAliasCharacters)) {
        return false;
    }
    auto boundedName = apc::limits::TruncateUtf16(deviceName, apc::limits::c_maxDeviceNameCharacters);

    bool changed = false;
    bool deviceExists = false;
    {
        auto locked = m_settings->LockExclusiveData();
        auto const device = std::ranges::find(locked->Devices, deviceId, &DeviceSettings::Id);
        if (device != locked->Devices.end()) {
            deviceExists = true;
            auto const nameChanged = !boundedName.empty() && device->Name != boundedName;
            auto const aliasChanged = device->Alias != alias;
            if (nameChanged || aliasChanged) {
                auto const index = static_cast<std::size_t>(std::distance(locked->Devices.begin(), device));
                auto& mutableDevice = locked.Mutate().Devices[index];
                if (nameChanged) mutableDevice.Name = boundedName;
                if (aliasChanged) mutableDevice.Alias = std::move(alias);
                changed = true;
            }
        }
        if (!deviceExists && !alias.empty() && locked->Devices.size() < apc::limits::c_maxPersistedDeviceCount) {
            auto const globalConnectOnStartup = locked->GlobalConnectOnStartup;
            auto const globalReconnectOnConnectionLoss = locked->GlobalReconnectOnConnectionLoss;
            locked.Mutate().Devices.push_back(
                DeviceSettings{.Id = deviceId,
                               .Name = std::move(boundedName),
                               .Alias = std::move(alias),
                               .ConnectOnStartup = globalConnectOnStartup,
                               .ReconnectOnConnectionLoss = globalReconnectOnConnectionLoss});
            deviceExists = true;
            changed = true;
        }
    }
    if (changed) {
        RequestSave();
        NotifyPresentationChanged();
    }
    return deviceExists;
}

void SettingsController::SetDefaultDeviceId(std::wstring const& deviceId) {
    if (!m_settings) return;
    if (deviceId.empty()) {
        ClearDefaultDevice();
        return;
    }
    if (!IsValidDeviceId(deviceId)) return;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->DefaultDevice != DefaultDeviceMode::SpecificDevice || locked->DefaultDeviceId != deviceId) {
            auto& data = locked.Mutate();
            data.DefaultDevice = DefaultDeviceMode::SpecificDevice;
            data.DefaultDeviceId = deviceId;
            changed = true;
        }
    }
    if (changed) {
        RequestSave();
        NotifyPresentationChanged();
    }
}

void SettingsController::ClearDefaultDevice() {
    if (!m_settings) return;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->DefaultDevice != DefaultDeviceMode::LastConnected || !locked->DefaultDeviceId.empty()) {
            auto& data = locked.Mutate();
            data.DefaultDevice = DefaultDeviceMode::LastConnected;
            data.DefaultDeviceId.clear();
            changed = true;
        }
    }
    if (changed) {
        RequestSave();
        NotifyPresentationChanged();
    }
}

std::size_t SettingsController::ConnectedDeviceCount() const {
    if (auto manager = m_deviceManager.lock()) {
        return manager->GetConnectedDevices().size();
    }
    return 0;
}

void SettingsController::ForgetDevice(std::wstring const& deviceId) {
    if (!m_settings || !IsValidDeviceId(deviceId)) return;
    bool changed = false;
    bool globalReconnectOnConnectionLoss = false;
    {
        auto locked = m_settings->LockExclusiveData();
        auto const hasDevice = std::ranges::contains(locked->Devices, deviceId, &DeviceSettings::Id);
        auto const hasLastConnected = std::ranges::contains(locked->LastConnectedIds, deviceId);
        auto const clearsDefault =
            locked->DefaultDevice == DefaultDeviceMode::SpecificDevice && locked->DefaultDeviceId == deviceId;
        changed = hasDevice || hasLastConnected || clearsDefault;
        globalReconnectOnConnectionLoss = locked->GlobalReconnectOnConnectionLoss;
        if (changed) {
            auto& data = locked.Mutate();
            std::erase_if(data.Devices, [&](auto const& device) { return device.Id == deviceId; });
            std::erase(data.LastConnectedIds, deviceId);
            if (clearsDefault) {
                data.DefaultDevice = DefaultDeviceMode::LastConnected;
                data.DefaultDeviceId.clear();
            }
        }
    }
    if (!changed) return;

    if (auto manager = m_deviceManager.lock()) {
        manager->SetReconnectOnConnectionLoss(winrt::hstring(deviceId), globalReconnectOnConnectionLoss);
    }
    RequestSave();
    NotifyPresentationChanged();
}

void SettingsController::NotifyPresentationChanged(PresentationChangeKind kind) {
    if (m_presentationChanged) {
        m_presentationChanged(kind);
    }
}

void SettingsController::RequestSave() {
    if (m_requestSave) {
        m_requestSave();
        return;
    }
    if (m_settings) m_settings->Save(GetModuleHandleW(nullptr));
}
