#pragma once

#include <core/SettingsData.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace apc::ui {
[[nodiscard]] std::wstring BuildSettingsDiagnosticsReport(SettingsData const& settings,
                                                          std::size_t connectedDeviceCount,
                                                          std::filesystem::path const& logPath,
                                                          std::wstring_view appVersionText);
}
