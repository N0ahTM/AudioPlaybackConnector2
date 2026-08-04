#include <algorithm>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <app/AutoReconnectPlanner.hpp>

#include <core/SettingsLimits.hpp>

std::vector<std::wstring> AutoReconnectPlanner::BuildReconnectPlan(SettingsData const& settings) {
    const auto deviceCount = std::min(settings.Devices.size(), apc::limits::c_maxPersistedDeviceCount);
    std::unordered_map<std::wstring_view, DeviceSettings const*> knownDevices;
    knownDevices.reserve(deviceCount);
    for (std::size_t index = 0; index < deviceCount; ++index) {
        auto const& device = settings.Devices[index];
        if (!device.Id.empty() && apc::limits::IsBoundedUtf16(device.Id, apc::limits::c_maxDeviceIdCharacters)) {
            knownDevices.emplace(device.Id, &device);
        }
    }

    std::vector<std::wstring> reconnectIds;
    reconnectIds.reserve(knownDevices.size());
    std::unordered_set<std::wstring_view> added;
    added.reserve(knownDevices.size());

    auto appendIfEligible = [&](std::wstring const& id) {
        if (id.empty() || !apc::limits::IsBoundedUtf16(id, apc::limits::c_maxDeviceIdCharacters)) return;
        const auto device = knownDevices.find(id);
        if (device != knownDevices.end() && (settings.GlobalConnectOnStartup || device->second->ConnectOnStartup) &&
            added.insert(id).second) {
            reconnectIds.push_back(id);
        }
    };

    const auto recentCount = std::min(settings.LastConnectedIds.size(), apc::limits::c_maxPersistedDeviceCount);
    for (std::size_t index = 0; index < recentCount; ++index) {
        appendIfEligible(settings.LastConnectedIds[index]);
    }
    for (std::size_t index = 0; index < deviceCount; ++index) {
        appendIfEligible(settings.Devices[index].Id);
    }

    return reconnectIds;
}

bool AutoReconnectPlanner::HasReconnectTargets(SettingsData const& settings) {
    const auto count = std::min(settings.Devices.size(), apc::limits::c_maxPersistedDeviceCount);
    return std::ranges::any_of(settings.Devices.begin(), settings.Devices.begin() + count, [&](auto const& device) {
        return !device.Id.empty() && apc::limits::IsBoundedUtf16(device.Id, apc::limits::c_maxDeviceIdCharacters) &&
               (settings.GlobalConnectOnStartup || device.ConnectOnStartup);
    });
}

bool AutoReconnectPlanner::PromoteMostRecentlyConnected(std::vector<std::wstring>& ids, std::wstring const& id) {
    if (id.empty() || !apc::limits::IsBoundedUtf16(id, apc::limits::c_maxDeviceIdCharacters)) return false;

    if (!ids.empty() && ids.front() == id && std::ranges::count(ids, id) == 1) return false;

    std::erase(ids, id);
    ids.insert(ids.begin(), id);
    if (ids.size() > apc::limits::c_maxPersistedDeviceCount) {
        ids.resize(apc::limits::c_maxPersistedDeviceCount);
    }
    return true;
}
