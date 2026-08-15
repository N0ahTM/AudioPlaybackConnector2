#pragma once

#include <core/DevicePickerSnapshot.hpp>
#include <memory>
#include <optional>

class DeviceManager;
class Settings;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Picker View Model //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DevicePickerViewModel {
public:
    using TimePoint = apc::device_picker::DevicePickerSnapshotCache::TimePoint;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void SetDeviceManager(std::weak_ptr<DeviceManager> manager);
    void SetSettings(std::weak_ptr<Settings> settings);
    void SetDevices(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
                    TimePoint refreshedAt = apc::device_picker::DevicePickerSnapshotCache::Clock::now());
    [[nodiscard]] bool SynchronizeInventoryFromManager(
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

    std::weak_ptr<DeviceManager> m_manager;
    std::weak_ptr<Settings> m_settings;
    apc::device_picker::DevicePickerSnapshotCache m_cache;
    std::optional<std::uint64_t> m_sourceInventoryGeneration;
};
