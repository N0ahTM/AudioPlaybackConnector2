#pragma once

#include <app/AppModels.hpp>
#include <string>
#include <string_view>

namespace apc::tray {
[[nodiscard]] std::wstring
BuildTooltip(std::wstring_view appName, std::wstring_view redactedDeviceName, apc::app::AppSnapshot const& snapshot);
} // namespace apc::tray
