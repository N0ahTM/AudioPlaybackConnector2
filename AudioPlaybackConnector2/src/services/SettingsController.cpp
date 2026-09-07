#include <pch.h>

#include <services/SettingsController.hpp>

#include <core/DeviceService.hpp>
#include <core/SettingsLimits.hpp>
#include <core/StringResources.hpp>

namespace {
[[nodiscard]] bool IsValidDeviceId(std::wstring_view value) noexcept {
    return !value.empty() && apc::limits::IsBoundedUtf16(value, apc::limits::c_maxDeviceIdCharacters);
}

[[nodiscard]] bool WasApplied(SettingsMutationResult const& result) noexcept {
    return result.Status == SettingsMutationStatus::Applied;
}
} // namespace

SettingsController::SettingsController(std::shared_ptr<SettingsStore> settings,
                                       std::weak_ptr<apc::device::DeviceService> deviceService)
    : m_settings(std::move(settings)), m_deviceService(std::move(deviceService)) {}

SettingsData SettingsController::Snapshot() const {
    return m_settings ? m_settings->Snapshot().Data : SettingsData{};
}

void SettingsController::SetPresentationChangedCallback(PresentationChangedCallback callback) {
    m_presentationChanged = std::move(callback);
}

void SettingsController::SetGlobalConnectOnStartup(bool enabled) {
    if (m_settings && WasApplied(m_settings->SetGlobalConnectOnStartup(enabled))) NotifyPresentationChanged();
}

void SettingsController::SetGlobalReconnectOnConnectionLoss(bool enabled) {
    if (!m_settings || !WasApplied(m_settings->SetGlobalReconnectOnConnectionLoss(enabled))) return;
    const auto snapshot = m_settings->Snapshot();
    if (auto service = m_deviceService.lock()) {
        std::vector<std::wstring> individuallyEnabledDeviceIds;
        for (auto const& device : snapshot.Data.Devices) {
            if (device.ReconnectOnConnectionLoss) individuallyEnabledDeviceIds.push_back(device.Id);
        }
        service->ApplyReconnectOnConnectionLossPolicy(snapshot.Data.GlobalReconnectOnConnectionLoss,
                                                      individuallyEnabledDeviceIds);
    }
    NotifyPresentationChanged();
}

void SettingsController::SetAllowIncomingConnections(bool enabled) {
    if (!m_settings || !WasApplied(m_settings->SetAllowIncomingConnections(enabled))) return;
    const auto snapshot = m_settings->Snapshot();
    if (auto service = m_deviceService.lock())
        service->SetIncomingConnectionsEnabled(snapshot.Data.AllowIncomingConnections);
}

void SettingsController::SetStartWithWindows(bool enabled) {
    if (m_settings) static_cast<void>(m_settings->SetStartWithWindows(enabled));
}

void SettingsController::SetShowNotifications(bool enabled) {
    if (m_settings) static_cast<void>(m_settings->SetShowNotifications(enabled));
}

void SettingsController::SetSystemBackdropEffects(bool enabled) {
    if (!m_settings || !WasApplied(m_settings->SetUseSystemBackdropEffects(enabled))) return;
    NotifyPresentationChanged(PresentationChangeKind::Appearance);
}

void SettingsController::SetLanguage(std::wstring language) {
    if (!m_settings) return;
    if (language.empty()) language = L"system";
    if (!WasApplied(m_settings->SetLanguage(language))) return;
    const auto snapshot = m_settings->Snapshot();
    StringResources::Instance().Initialize(GetModuleHandleW(nullptr), snapshot.Data.Language);
    NotifyPresentationChanged(PresentationChangeKind::Language);
}

void SettingsController::SetPrivacyMode(bool enabled) {
    if (!m_settings || !WasApplied(m_settings->SetPrivacyModeEnabled(enabled))) return;
    NotifyPresentationChanged();
}

bool SettingsController::SetSettingsWindowBounds(PersistedWindowBounds bounds) {
    return m_settings && WasApplied(m_settings->SetSettingsWindowBounds(bounds));
}

bool SettingsController::ClearSettingsWindowBounds() {
    return m_settings && WasApplied(m_settings->SetSettingsWindowBounds(std::nullopt));
}

void SettingsController::SetDeviceConnectOnStartup(std::wstring const& deviceId, bool enabled) {
    if (RememberKnownDevice(deviceId) && WasApplied(m_settings->SetDeviceConnectOnStartup(deviceId, enabled)))
        NotifyPresentationChanged();
}

void SettingsController::SetDeviceReconnectOnConnectionLoss(std::wstring const& deviceId, bool enabled) {
    if (!RememberKnownDevice(deviceId) ||
        !WasApplied(m_settings->SetDeviceReconnectOnConnectionLoss(deviceId, enabled)))
        return;
    const auto snapshot = m_settings->Snapshot();
    if (auto service = m_deviceService.lock()) {
        const auto device = std::ranges::find(snapshot.Data.Devices, deviceId, &DeviceSettings::Id);
        if (device != snapshot.Data.Devices.end()) {
            service->SetReconnectOnConnectionLoss(
                deviceId, snapshot.Data.GlobalReconnectOnConnectionLoss || device->ReconnectOnConnectionLoss);
        }
    }
    NotifyPresentationChanged();
}

bool SettingsController::SetDeviceAlias(std::wstring const& deviceId,
                                        std::wstring alias,
                                        std::wstring const& deviceName) {
    if (!m_settings || !IsValidDeviceId(deviceId)) return false;
    const auto boundedName = apc::limits::TruncateUtf16(deviceName, apc::limits::c_maxDeviceNameCharacters);
    const auto result = m_settings->SetDeviceAlias(deviceId, alias, boundedName);
    if (WasApplied(result.Mutation)) NotifyPresentationChanged();
    return result.DeviceExists;
}

bool SettingsController::SetDefaultDeviceId(std::wstring const& deviceId) {
    if (!m_settings) return false;
    if (!deviceId.empty() && !RememberKnownDevice(deviceId)) return false;
    const auto result = deviceId.empty() ? m_settings->ClearDefaultDevice() : m_settings->SetDefaultDevice(deviceId);
    if (WasApplied(result)) NotifyPresentationChanged();
    return result.Status != SettingsMutationStatus::Rejected;
}

bool SettingsController::ClearDefaultDevice() {
    if (!m_settings) return false;
    const auto result = m_settings->ClearDefaultDevice();
    if (WasApplied(result)) NotifyPresentationChanged();
    return result.Status != SettingsMutationStatus::Rejected;
}

std::size_t SettingsController::ConnectedDeviceCount() const {
    if (auto service = m_deviceService.lock()) return service->GetConnectedDevices().size();
    return 0;
}

void SettingsController::ForgetDevice(std::wstring const& deviceId) {
    if (!m_settings || !IsValidDeviceId(deviceId) || !WasApplied(m_settings->ForgetDevice(deviceId))) return;
    const auto snapshot = m_settings->Snapshot();
    if (auto service = m_deviceService.lock()) {
        service->SetReconnectOnConnectionLoss(deviceId, snapshot.Data.GlobalReconnectOnConnectionLoss);
    }
    NotifyPresentationChanged();
}

void SettingsController::NotifyPresentationChanged(PresentationChangeKind kind) {
    if (m_presentationChanged) m_presentationChanged(kind);
}

bool SettingsController::RememberKnownDevice(std::wstring const& deviceId) {
    if (!m_settings || !IsValidDeviceId(deviceId)) return false;
    const auto settings = m_settings->Snapshot();
    if (std::ranges::find(settings.Data.Devices, deviceId, &DeviceSettings::Id) != settings.Data.Devices.end())
        return true;
    auto service = m_deviceService.lock();
    if (!service) return false;
    const auto inventory = service->GetDevicePickerInventorySnapshot();
    const auto device = std::ranges::find(inventory.Devices, deviceId, &apc::device_picker::DeviceIdentity::Id);
    if (device == inventory.Devices.end()) return false;
    static_cast<void>(m_settings->RememberDevice(
        deviceId, apc::limits::TruncateUtf16(device->Name, apc::limits::c_maxDeviceNameCharacters)));
    const auto updated = m_settings->Snapshot();
    return std::ranges::find(updated.Data.Devices, deviceId, &DeviceSettings::Id) != updated.Data.Devices.end();
}
