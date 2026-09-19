#pragma once

#include <app/AppModels.hpp>
#include <core/DevicePickerTypes.hpp>
#include <ui/SettingsDeviceViewModel.hpp>
#include <optional>
#include <string_view>

struct DeviceOptionsViewModel {
    SettingsDeviceViewModel Device;
    bool GlobalConnectOnStartup = false;
    bool GlobalReconnectOnConnectionLoss = false;
    bool CanForget = false;
};

struct DevicePickerViewState {
    std::uint64_t Generation = 0;
    std::size_t ConnectedDeviceCount = 0;
    bool InventoryComplete = false;
    std::vector<apc::device_picker::DeviceSnapshotItem> Items;
};

// Localization is an explicit value so these projections have no service or global-state dependency.
[[nodiscard]] DevicePickerViewState BuildDevicePickerViewState(apc::app::AppSnapshot const& snapshot,
                                                               std::wstring_view privateDeviceName);
[[nodiscard]] std::optional<DeviceOptionsViewModel> BuildDeviceOptionsViewState(apc::app::AppSnapshot const& snapshot,
                                                                                std::wstring_view id,
                                                                                std::wstring_view privateDeviceName);
