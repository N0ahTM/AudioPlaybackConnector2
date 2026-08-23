#pragma once

#include <core/DevicePickerSnapshot.hpp>
#include <memory>
#include <optional>

class SettingsStore;

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
    apc::device_picker::DevicePickerSnapshotCache m_cache;
    std::optional<std::uint64_t> m_sourceInventoryGeneration;
    bool m_sourceEnumerationComplete = false;
};
