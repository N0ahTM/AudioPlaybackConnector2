#pragma once

#include <core/SettingsData.hpp>
#include <string>
#include <string_view>

namespace apc::settings {
struct DecodedSettings {
    SettingsData Data;
    bool NeedsRewrite = false;
};

// Decode remains strict for the current format. Load also accepts the 0.9.1
// unversioned format and marks it for an atomic rewrite as schema 2.
[[nodiscard]] SettingsData Decode(std::string_view bytes);
[[nodiscard]] DecodedSettings DecodePersisted(std::string_view bytes);
[[nodiscard]] std::string Encode(SettingsData const& data);
} // namespace apc::settings
