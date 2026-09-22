#include <core/SettingsCodec.hpp>
#include <core/SettingsLimits.hpp>
#include <nlohmann/json.hpp>
#include <winrt/base.h>

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace apc::settings {
namespace {
using Json = nlohmann::json;
constexpr int c_schemaVersion = 2;

constexpr std::string_view c_topLevelKeys[] = {"schemaVersion",
                                               "globalConnectOnStartup",
                                               "globalReconnectOnConnectionLoss",
                                               "allowIncomingConnections",
                                               "startWithWindows",
                                               "showNotifications",
                                               "useSystemBackdropEffects",
                                               "privacyModeEnabled",
                                               "language",
                                               "defaultDeviceMode",
                                               "defaultDeviceId",
                                               "settingsWindowBounds",
                                               "devices",
                                               "lastConnectedIds",
                                               "ratingPrompt"};
constexpr std::string_view c_windowBoundsKeys[] = {"x", "y", "width", "height", "dpi"};
constexpr std::string_view c_deviceKeys[] = {"id", "name", "alias", "connectOnStartup", "reconnectOnConnectionLoss"};
constexpr std::string_view c_ratingPromptKeys[] = {
    "firstLaunchDate", "usageDays", "lastUsageDate", "state", "deferUntil", "deferCount"};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Validation ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void Require(bool valid) {
    if (!valid) throw std::invalid_argument("invalid or unsupported settings format");
}

void Object(Json const& value, std::span<const std::string_view> keys) {
    Require(value.is_object());
    for (auto const& [key, ignored] : value.items()) {
        (void)ignored;
        Require(std::ranges::find(keys, key) != keys.end());
    }
}

std::wstring Text(Json const& object, char const* key, std::wstring_view fallback = {}) {
    auto found = object.find(key);
    if (found == object.end()) return std::wstring(fallback);
    Require(found->is_string());
    auto const& text = found->get_ref<std::string const&>();
    return std::wstring(winrt::to_hstring(text));
}

std::int32_t Integer(Json const& object, char const* key, std::int32_t fallback) {
    auto found = object.find(key);
    if (found == object.end()) return fallback;
    Require(found->is_number_integer());
    if (found->is_number_unsigned()) {
        Require(found->get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
    } else {
        auto const number = found->get<std::int64_t>();
        Require(number >= std::numeric_limits<std::int32_t>::min() &&
                number <= std::numeric_limits<std::int32_t>::max());
    }
    return found->get<std::int32_t>();
}

Json const& Array(Json const& object, char const* key) {
    static Json const empty = Json::array();
    auto found = object.find(key);
    if (found == object.end()) return empty;
    Require(found->is_array() && found->size() <= apc::limits::c_maxPersistedDeviceCount);
    return *found;
}

bool IsPersistable(SettingsData const& data) {
    if ((data.DefaultDevice != DefaultDeviceMode::LastConnected &&
         data.DefaultDevice != DefaultDeviceMode::SpecificDevice) ||
        data.Devices.size() > apc::limits::c_maxPersistedDeviceCount ||
        data.LastConnectedIds.size() > apc::limits::c_maxPersistedDeviceCount ||
        !apc::limits::IsSupportedLanguage(data.Language) ||
        !apc::limits::IsBoundedUtf16(data.DefaultDeviceId, apc::limits::c_maxDeviceIdCharacters) ||
        ((data.DefaultDevice == DefaultDeviceMode::SpecificDevice) != !data.DefaultDeviceId.empty())) {
        return false;
    }
    if (data.SettingsWindowBounds && (data.SettingsWindowBounds->Width <= 0 || data.SettingsWindowBounds->Height <= 0 ||
                                      data.SettingsWindowBounds->Dpi < apc::limits::c_minWindowDpi ||
                                      data.SettingsWindowBounds->Dpi > apc::limits::c_maxWindowDpi)) {
        return false;
    }
    std::unordered_set<std::wstring_view> deviceIds;
    std::unordered_set<std::wstring_view> connectedIds;
    for (auto const& device : data.Devices) {
        if (device.Id.empty() || !apc::limits::IsBoundedUtf16(device.Id, apc::limits::c_maxDeviceIdCharacters) ||
            !apc::limits::IsBoundedUtf16(device.Name, apc::limits::c_maxDeviceNameCharacters) ||
            !apc::limits::IsBoundedUtf16(device.Alias, apc::limits::c_maxDeviceAliasCharacters) ||
            !deviceIds.insert(device.Id).second) {
            return false;
        }
    }
    for (auto const& id : data.LastConnectedIds) {
        if (id.empty() || !apc::limits::IsBoundedUtf16(id, apc::limits::c_maxDeviceIdCharacters) ||
            !connectedIds.insert(id).second) {
            return false;
        }
    }
    return true;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Current Format ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

SettingsData Decode(std::string_view bytes) try {
    Require(!bytes.empty() && bytes.size() <= apc::limits::c_maxSettingsFileBytes);
    // Reject duplicate object keys rather than silently selecting the last value.
    std::vector<std::unordered_set<std::string>> objectKeys;
    auto const json = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 16);
        if (event == Json::parse_event_t::object_start) objectKeys.emplace_back();
        if (event == Json::parse_event_t::key) Require(objectKeys.back().insert(value.get<std::string>()).second);
        if (event == Json::parse_event_t::object_end) objectKeys.pop_back();
        return true;
    });
    Object(json, c_topLevelKeys);
    Require(Integer(json, "schemaVersion", -1) == c_schemaVersion);
    SettingsData data;
    data.GlobalConnectOnStartup = json.value("globalConnectOnStartup", false);
    data.GlobalReconnectOnConnectionLoss = json.value("globalReconnectOnConnectionLoss", false);
    data.AllowIncomingConnections = json.value("allowIncomingConnections", false);
    data.StartWithWindows = json.value("startWithWindows", false);
    data.ShowNotifications = json.value("showNotifications", true);
    data.UseSystemBackdropEffects = json.value("useSystemBackdropEffects", true);
    data.PrivacyModeEnabled = json.value("privacyModeEnabled", false);
    data.Language = Text(json, "language", L"system");
    auto const mode = Text(json, "defaultDeviceMode", L"lastConnected");
    Require(mode == L"lastConnected" || mode == L"specificDevice");
    data.DefaultDevice =
        mode == L"specificDevice" ? DefaultDeviceMode::SpecificDevice : DefaultDeviceMode::LastConnected;
    data.DefaultDeviceId = Text(json, "defaultDeviceId");
    if (auto bounds = json.find("settingsWindowBounds"); bounds != json.end()) {
        Object(*bounds, c_windowBoundsKeys);
        auto const dpi = Integer(*bounds, "dpi", USER_DEFAULT_SCREEN_DPI);
        Require(dpi > 0);
        data.SettingsWindowBounds = PersistedWindowBounds{Integer(*bounds, "x", 0),
                                                          Integer(*bounds, "y", 0),
                                                          Integer(*bounds, "width", 0),
                                                          Integer(*bounds, "height", 0),
                                                          static_cast<std::uint32_t>(dpi)};
    }
    for (auto const& row : Array(json, "devices")) {
        Object(row, c_deviceKeys);
        data.Devices.push_back({Text(row, "id"),
                                Text(row, "name"),
                                Text(row, "alias"),
                                row.value("connectOnStartup", false),
                                row.value("reconnectOnConnectionLoss", false)});
    }
    for (auto const& id : Array(json, "lastConnectedIds")) {
        Require(id.is_string());
        data.LastConnectedIds.emplace_back(winrt::to_hstring(id.get_ref<std::string const&>()));
    }
    if (auto rating = json.find("ratingPrompt"); rating != json.end()) {
        Object(*rating, c_ratingPromptKeys);
        auto& prompt = data.RatingPrompt;
        prompt.FirstLaunchDate = Text(*rating, "firstLaunchDate");
        prompt.LastUsageDate = Text(*rating, "lastUsageDate");
        prompt.DeferUntil = Text(*rating, "deferUntil");
        prompt.UsageDays = Integer(*rating, "usageDays", 0);
        prompt.DeferCount = Integer(*rating, "deferCount", 0);
        Require(prompt.UsageDays >= 0 && prompt.DeferCount >= 0);
        const auto state = Text(*rating, "state", L"idle");
        if (state == L"idle")
            prompt.State = RatingPromptState::Idle;
        else if (state == L"deferred")
            prompt.State = RatingPromptState::Deferred;
        else if (state == L"completed")
            prompt.State = RatingPromptState::Completed;
        else if (state == L"disabled")
            prompt.State = RatingPromptState::Disabled;
        else
            Require(false);
        for (auto const& date : {prompt.FirstLaunchDate, prompt.LastUsageDate, prompt.DeferUntil}) {
            if (date.empty()) continue;
            bool valid = date.size() == 10 && date[4] == L'-' && date[7] == L'-';
            for (std::size_t i = 0; valid && i < date.size(); ++i) {
                if (i == 4 || i == 7) continue;
                valid = date[i] >= L'0' && date[i] <= L'9';
            }
            if (valid) {
                const auto month = std::stoi(std::wstring(date.substr(5, 2)));
                const auto day = std::stoi(std::wstring(date.substr(8, 2)));
                valid = month >= 1 && month <= 12 && day >= 1 && day <= 31;
            }
            Require(valid);
        }
    }
    Require(IsPersistable(data));
    return data;
} catch (Json::exception const&) {
    throw std::invalid_argument("invalid settings JSON");
}

std::string Encode(SettingsData const& data) {
    Require(IsPersistable(data));
    Json json = {{"schemaVersion", c_schemaVersion},
                 {"globalConnectOnStartup", data.GlobalConnectOnStartup},
                 {"globalReconnectOnConnectionLoss", data.GlobalReconnectOnConnectionLoss},
                 {"allowIncomingConnections", data.AllowIncomingConnections},
                 {"startWithWindows", data.StartWithWindows},
                 {"showNotifications", data.ShowNotifications},
                 {"useSystemBackdropEffects", data.UseSystemBackdropEffects},
                 {"privacyModeEnabled", data.PrivacyModeEnabled},
                 {"language", winrt::to_string(data.Language)},
                 {"defaultDeviceMode",
                  data.DefaultDevice == DefaultDeviceMode::SpecificDevice ? "specificDevice" : "lastConnected"},
                 {"defaultDeviceId", winrt::to_string(data.DefaultDeviceId)},
                 {"devices", Json::array()},
                 {"lastConnectedIds", Json::array()}};
    if (auto const& bounds = data.SettingsWindowBounds) {
        json["settingsWindowBounds"] = {{"x", bounds->X},
                                        {"y", bounds->Y},
                                        {"width", bounds->Width},
                                        {"height", bounds->Height},
                                        {"dpi", bounds->Dpi}};
    }
    if (!(data.RatingPrompt == RatingPromptData{})) {
        const auto& prompt = data.RatingPrompt;
        const char* state = "idle";
        switch (prompt.State) {
            case RatingPromptState::Deferred: state = "deferred"; break;
            case RatingPromptState::Completed: state = "completed"; break;
            case RatingPromptState::Disabled: state = "disabled"; break;
            default: break;
        }
        json["ratingPrompt"] = {{"firstLaunchDate", winrt::to_string(prompt.FirstLaunchDate)},
                                {"usageDays", prompt.UsageDays},
                                {"lastUsageDate", winrt::to_string(prompt.LastUsageDate)},
                                {"state", state},
                                {"deferUntil", winrt::to_string(prompt.DeferUntil)},
                                {"deferCount", prompt.DeferCount}};
    }
    for (auto const& device : data.Devices) {
        json["devices"].push_back({{"id", winrt::to_string(device.Id)},
                                   {"name", winrt::to_string(device.Name)},
                                   {"alias", winrt::to_string(device.Alias)},
                                   {"connectOnStartup", device.ConnectOnStartup},
                                   {"reconnectOnConnectionLoss", device.ReconnectOnConnectionLoss}});
    }
    for (auto const& id : data.LastConnectedIds)
        json["lastConnectedIds"].push_back(winrt::to_string(id));
    auto bytes = json.dump();
    Require(bytes.size() <= apc::limits::c_maxSettingsFileBytes);
    return bytes;
}
} // namespace apc::settings
