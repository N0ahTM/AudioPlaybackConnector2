#pragma once

#include <core/Settings.hpp>

class AutoReconnectPlanner final {
public:
    [[nodiscard]] static std::vector<std::wstring> BuildReconnectPlan(SettingsData const& settings);
    [[nodiscard]] static bool HasReconnectTargets(SettingsData const& settings);
    [[nodiscard]] static bool PromoteMostRecentlyConnected(std::vector<std::wstring>& ids, std::wstring const& id);
};
