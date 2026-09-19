#pragma once

#include <core/SettingsData.hpp>
#include <string>
#include <string_view>

namespace apc::settings {
// Only the current format is supported. Invalid or unsupported input throws
// std::invalid_argument; the store preserves the original before using defaults.
[[nodiscard]] SettingsData Decode(std::string_view bytes);
[[nodiscard]] std::string Encode(SettingsData const& data);
} // namespace apc::settings
