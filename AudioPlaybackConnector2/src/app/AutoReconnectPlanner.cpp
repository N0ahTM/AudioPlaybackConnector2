#include <pch.h>
#include <app/AutoReconnectPlanner.hpp>

std::vector<std::wstring> AutoReconnectPlanner::BuildReconnectPlan(SettingsData const& settings) {
    std::vector<std::wstring> reconnectIds;
    reconnectIds.reserve(settings.LastConnectedIds.size());

    for (auto const& id : settings.LastConnectedIds) {
        auto device =
            std::ranges::find_if(settings.Devices, [&](auto const& knownDevice) { return knownDevice.Id == id; });
        if (device != settings.Devices.end() && (settings.GlobalConnectOnStartup || device->ConnectOnStartup)) {
            reconnectIds.push_back(id);
        }
    }

    return reconnectIds;
}

bool AutoReconnectPlanner::HasReconnectTargets(SettingsData const& settings) {
    return !BuildReconnectPlan(settings).empty();
}
