#pragma once

#include <core/DevicePickerSnapshot.hpp>
#include <app/SettingsWindowCommandExecutor.hpp>
#include <ui/SettingsDeviceViewModel.hpp>
#include <memory>
#include <optional>

class SettingsStore;
class ISettingsController;

struct DeviceOptionsViewModel {
    SettingsDeviceViewModel Device;
    bool GlobalConnectOnStartup = false;
    bool GlobalReconnectOnConnectionLoss = false;
    bool CanForget = false;
};

namespace apc::device {
class DeviceService;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Picker View Model //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DevicePickerViewModel {
public:
    using TimePoint = apc::device_picker::DevicePickerSnapshotCache::TimePoint;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetDeviceService(std::weak_ptr<apc::device::DeviceService> service);
    void SetSettingsStore(std::weak_ptr<SettingsStore> settingsStore);
    void SetDeviceSettings(std::weak_ptr<ISettingsController> controller,
                           apc::app::SettingsWindowCommandExecutor::ExecuteCallback execute);
    [[nodiscard]] std::optional<DeviceOptionsViewModel> DeviceOptions(std::wstring_view id) const;
    [[nodiscard]] bool SetAlias(std::wstring_view id, std::wstring_view alias) const;
    [[nodiscard]] bool SetDefault(std::wstring_view id, bool enabled) const;
    [[nodiscard]] bool SetConnectOnStartup(std::wstring const& id, bool enabled) const;
    [[nodiscard]] bool SetReconnectOnConnectionLoss(std::wstring const& id, bool enabled) const;
    [[nodiscard]] bool ForgetDevice(std::wstring const& id) const;
    void SetDevices(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
                    TimePoint refreshedAt = apc::device_picker::DevicePickerSnapshotCache::Clock::now());
    [[nodiscard]] bool SynchronizeInventoryFromService(
        TimePoint refreshedAt = apc::device_picker::DevicePickerSnapshotCache::Clock::now());
    void InvalidateInventory() noexcept;
    void Clear() noexcept;
    [[nodiscard]] bool HasInventory() const noexcept;
    [[nodiscard]] bool
    IsInventoryFresh(TimePoint now = apc::device_picker::DevicePickerSnapshotCache::Clock::now()) const noexcept;
    [[nodiscard]] apc::device_picker::DevicePickerSnapshot const&
    RefreshSnapshot(TimePoint now = apc::device_picker::DevicePickerSnapshotCache::Clock::now());
    [[nodiscard]] apc::device_picker::DevicePickerSnapshot const& CachedSnapshot() const noexcept;
    [[nodiscard]] bool CanSelect(winrt::hstring const& id) const noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::weak_ptr<apc::device::DeviceService> m_service;
    std::weak_ptr<SettingsStore> m_settingsStore;
    std::weak_ptr<ISettingsController> m_settingsController;
    std::optional<apc::app::SettingsWindowCommandExecutor> m_deviceCommands;
    apc::device_picker::DevicePickerSnapshotCache m_cache;
    std::optional<std::uint64_t> m_sourceInventoryGeneration;
    bool m_sourceEnumerationComplete = false;
};
