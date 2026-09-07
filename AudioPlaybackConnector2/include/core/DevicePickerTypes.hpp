#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace apc::device_picker {

struct DeviceIdentity {
    std::wstring Id;
    std::wstring Name;
};

struct DeviceInventorySnapshot {
    std::uint64_t Generation = 0;
    bool EnumerationComplete = false;
    std::vector<DeviceIdentity> Devices;
};

struct DevicePresentationSetting {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    bool IsDefault = false;
};

struct DeviceActivitySnapshot {
    std::unordered_set<std::wstring> ConnectedIds;
    std::unordered_set<std::wstring> BusyIds;
};

struct DeviceSnapshotItem {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    bool IsConnected = false;
    bool IsBusy = false;
    bool IsAvailable = true;
    bool IsDefault = false;

    bool operator==(DeviceSnapshotItem const&) const = default;
};

struct DevicePickerSnapshot {
    std::uint64_t Generation = 0;
    std::uint64_t InventoryGeneration = 0;
    std::size_t ConnectedDeviceCount = 0;
    bool InventoryFresh = false;
    bool PrivacyModeEnabled = false;
    std::vector<DeviceSnapshotItem> Items;
};

} // namespace apc::device_picker
