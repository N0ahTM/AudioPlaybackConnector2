#pragma once

#include <cstdint>
#include <string>
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

} // namespace apc::device_picker
