#include <algorithm>
#include <ranges>

#include <app/AutoReconnectPlanner.hpp>

std::vector<std::wstring> AutoReconnectPlanner::BuildReconnectPlan(SettingsData const& settings) {
    std::vector<std::wstring> reconnectIds;
    reconnectIds.reserve(settings.Devices.size());

    auto appendIfEligible = [&](std::wstring const& id) {
        if (id.empty() || std::ranges::find(reconnectIds, id) != reconnectIds.end()) return;

        auto device =
            std::ranges::find_if(settings.Devices, [&](auto const& knownDevice) { return knownDevice.Id == id; });
        if (device != settings.Devices.end() && (settings.GlobalConnectOnStartup || device->ConnectOnStartup)) {
            reconnectIds.push_back(id);
        }
    };

    for (auto const& id : settings.LastConnectedIds) {
        appendIfEligible(id);
    }
    for (auto const& device : settings.Devices) {
        appendIfEligible(device.Id);
    }

    return reconnectIds;
}

bool AutoReconnectPlanner::HasReconnectTargets(SettingsData const& settings) {
    return !BuildReconnectPlan(settings).empty();
}

bool AutoReconnectPlanner::PromoteMostRecentlyConnected(std::vector<std::wstring>& ids, std::wstring const& id) {
    if (id.empty()) return false;

    if (!ids.empty() && ids.front() == id && std::ranges::count(ids, id) == 1) return false;

    std::erase(ids, id);
    ids.insert(ids.begin(), id);
    return true;
}
