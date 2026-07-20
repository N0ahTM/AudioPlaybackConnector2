#include <pch.h>

#include <services/SettingsController.hpp>

#include <core/DeviceManager.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsController::SettingsController(std::shared_ptr<Settings> settings, std::weak_ptr<DeviceManager> deviceManager)
    : m_settings(std::move(settings)), m_deviceManager(std::move(deviceManager)) {}

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

void SettingsController::SetGlobalAutoReconnect(bool enabled) {
    if (!m_settings) return;

    std::vector<DeviceSettings> devices;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->GlobalAutoReconnect == enabled) return;
        locked->GlobalAutoReconnect = enabled;
        devices = locked->Devices;
    }

    if (auto manager = m_deviceManager.lock()) {
        for (const auto& connection : manager->GetConnectedDevices()) {
            bool autoReconnect = enabled;
            for (const auto& device : devices) {
                if (device.Id == connection.Id) {
                    autoReconnect = autoReconnect || device.AutoReconnect;
                    break;
                }
            }
            manager->SetAutoReconnect(winrt::hstring(connection.Id), autoReconnect);
        }
    }

    m_settings->Save(GetModuleHandleW(nullptr));
}

void SettingsController::SetStartWithWindows(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->StartWithWindows == enabled) return;
        locked->StartWithWindows = enabled;
    }
    m_settings->Save(GetModuleHandleW(nullptr));
}

void SettingsController::SetShowNotifications(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->ShowNotifications == enabled) return;
        locked->ShowNotifications = enabled;
    }
    m_settings->Save(GetModuleHandleW(nullptr));
}

void SettingsController::SetLanguage(std::wstring language) {
    if (!m_settings) return;
    if (language.empty()) {
        language = L"system";
    }
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->Language == language) return;
        locked->Language = std::move(language);
    }
    m_settings->Save(GetModuleHandleW(nullptr));
    NotifyPresentationChanged();
}

void SettingsController::SetPrivacyMode(bool enabled) {
    if (!m_settings) return;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->PrivacyModeEnabled == enabled) return;
        locked->PrivacyModeEnabled = enabled;
    }
    m_settings->Save(GetModuleHandleW(nullptr));
    NotifyPresentationChanged();
}

bool SettingsController::SetSettingsWindowBounds(PersistedWindowBounds bounds) {
    if (!m_settings) return false;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->SettingsWindowBounds != bounds) {
            locked->SettingsWindowBounds = bounds;
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
            locked->SettingsWindowBounds.reset();
            changed = true;
        }
    }

    return changed;
}

void SettingsController::Save() {
    if (m_settings) {
        m_settings->Save(GetModuleHandleW(nullptr));
    }
}

void SettingsController::SetDeviceAutoReconnect(std::wstring const& deviceId, bool enabled) {
    if (!m_settings) return;

    bool changed = false;
    bool globalAutoReconnect = false;
    {
        auto locked = m_settings->LockExclusiveData();
        for (auto& device : locked->Devices) {
            if (device.Id == deviceId) {
                if (device.AutoReconnect == enabled) return;
                device.AutoReconnect = enabled;
                changed = true;
                break;
            }
        }
        globalAutoReconnect = locked->GlobalAutoReconnect;
    }
    if (!changed) return;

    if (auto manager = m_deviceManager.lock()) {
        manager->SetAutoReconnect(winrt::hstring(deviceId), globalAutoReconnect || enabled);
    }

    m_settings->Save(GetModuleHandleW(nullptr));
}

bool SettingsController::SetDeviceAlias(std::wstring const& deviceId,
                                        std::wstring alias,
                                        std::wstring const& deviceName) {
    if (!m_settings || deviceId.empty()) return false;

    bool changed = false;
    bool deviceExists = false;
    {
        auto locked = m_settings->LockExclusiveData();
        for (auto& device : locked->Devices) {
            if (device.Id == deviceId) {
                deviceExists = true;
                if (!deviceName.empty() && device.Name != deviceName) {
                    device.Name = deviceName;
                    changed = true;
                }
                if (device.Alias != alias) {
                    device.Alias = std::move(alias);
                    changed = true;
                }
                break;
            }
        }
        if (!deviceExists && !alias.empty()) {
            locked->Devices.push_back(DeviceSettings{.Id = deviceId,
                                                     .Name = deviceName,
                                                     .Alias = std::move(alias),
                                                     .AutoReconnect = locked->GlobalAutoReconnect});
            deviceExists = true;
            changed = true;
        }
    }
    if (changed) {
        m_settings->Save(GetModuleHandleW(nullptr));
        NotifyPresentationChanged();
    }
    return deviceExists;
}

void SettingsController::SetDefaultDeviceMode(DefaultDeviceMode mode) {
    if (!m_settings) return;

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->DefaultDevice != mode) {
            locked->DefaultDevice = mode;
            changed = true;
        }
        if (mode == DefaultDeviceMode::LastConnected && !locked->DefaultDeviceId.empty()) {
            locked->DefaultDeviceId.clear();
            changed = true;
        }
    }
    if (changed) {
        m_settings->Save(GetModuleHandleW(nullptr));
        NotifyPresentationChanged();
    }
}

void SettingsController::SetDefaultDeviceId(std::wstring const& deviceId) {
    if (!m_settings) return;
    if (deviceId.empty()) {
        SetDefaultDeviceMode(DefaultDeviceMode::LastConnected);
        return;
    }

    bool changed = false;
    {
        auto locked = m_settings->LockExclusiveData();
        if (locked->DefaultDevice != DefaultDeviceMode::SpecificDevice) {
            locked->DefaultDevice = DefaultDeviceMode::SpecificDevice;
            changed = true;
        }
        if (locked->DefaultDeviceId != deviceId) {
            locked->DefaultDeviceId = deviceId;
            changed = true;
        }
    }
    if (changed) {
        m_settings->Save(GetModuleHandleW(nullptr));
        NotifyPresentationChanged();
    }
}

void SettingsController::ClearDefaultDevice() {
    SetDefaultDeviceMode(DefaultDeviceMode::LastConnected);
}

std::size_t SettingsController::ConnectedDeviceCount() const {
    if (auto manager = m_deviceManager.lock()) {
        return manager->GetConnectedDevices().size();
    }
    return 0;
}

void SettingsController::ForgetDevice(std::wstring const& deviceId) {
    if (!m_settings) return;
    bool changed = false;
    bool globalAutoReconnect = false;
    {
        auto locked = m_settings->LockExclusiveData();
        const auto devicesRemoved =
            std::erase_if(locked->Devices, [&](auto const& device) { return device.Id == deviceId; });
        const auto lastConnectedRemoved = std::erase(locked->LastConnectedIds, deviceId);
        if (locked->DefaultDevice == DefaultDeviceMode::SpecificDevice && locked->DefaultDeviceId == deviceId) {
            locked->DefaultDevice = DefaultDeviceMode::LastConnected;
            locked->DefaultDeviceId.clear();
            changed = true;
        }
        changed = changed || devicesRemoved > 0 || lastConnectedRemoved > 0;
        globalAutoReconnect = locked->GlobalAutoReconnect;
    }
    if (!changed) return;

    if (auto manager = m_deviceManager.lock()) {
        manager->SetAutoReconnect(winrt::hstring(deviceId), globalAutoReconnect);
    }
    m_settings->Save(GetModuleHandleW(nullptr));
    NotifyPresentationChanged();
}

void SettingsController::NotifyPresentationChanged() {
    if (m_presentationChanged) {
        m_presentationChanged();
    }
}
