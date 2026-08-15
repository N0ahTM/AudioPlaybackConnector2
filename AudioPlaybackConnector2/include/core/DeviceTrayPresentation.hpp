#pragma once

#include <string>
#include <vector>

struct DeviceTrayPresentationItem {
    std::wstring Id;
    std::wstring Name;

    bool operator==(DeviceTrayPresentationItem const&) const = default;
};

struct DeviceTrayPresentationSnapshot {
    std::vector<DeviceTrayPresentationItem> ConnectedDevices;
    bool HasBusyOperations = false;
};
