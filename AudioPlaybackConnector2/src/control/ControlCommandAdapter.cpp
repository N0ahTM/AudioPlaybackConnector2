#include <control/ControlCommandAdapter.hpp>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using apc::app::AppCommand;
using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppOutcomeReason;
using apc::app::AppResult;
using apc::app::AppResultCode;
using apc::app::AppSnapshot;
using apc::app::DeviceSelector;
using apc::app::DeviceSelectorKind;
using apc::control::CommandFlagJson;
using apc::control::CommandFlagRaw;
using apc::control::CommandType;
using apc::control::ControlCommandAdapter;
using apc::control::ExitCode;
using apc::control::Request;
using apc::control::Response;
using apc::control::TargetKind;
using JsonArray = winrt::Windows::Data::Json::JsonArray;
using JsonObject = winrt::Windows::Data::Json::JsonObject;
using JsonValue = winrt::Windows::Data::Json::JsonValue;

std::wstring KeyText(std::string_view key) {
    return std::wstring(key.begin(), key.end());
}

std::wstring Resource(ControlCommandAdapter::Localize const& localize, std::string_view key) {
    if (localize) return localize(key);
    return KeyText(key);
}

std::wstring Replace(std::wstring value, std::wstring_view placeholder, std::wstring_view replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(placeholder, offset)) != std::wstring::npos) {
        value.replace(offset, placeholder.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::wstring
FormatResource(ControlCommandAdapter::Localize const& localize, std::string_view key, std::wstring_view first) {
    return Replace(Resource(localize, key), L"{0}", first);
}

std::wstring FormatResource(ControlCommandAdapter::Localize const& localize,
                            std::string_view key,
                            std::wstring_view first,
                            std::wstring_view second) {
    auto result = FormatResource(localize, key, first);
    return Replace(std::move(result), L"{1}", second);
}

std::wstring FormatResource(ControlCommandAdapter::Localize const& localize, std::string_view key, std::size_t value) {
    return FormatResource(localize, key, std::to_wstring(value));
}

bool IsRequestValidForAdapter(Request const& request) noexcept {
    return apc::control::IsRequestValid(request);
}

std::optional<DeviceSelector> MakeSelector(TargetKind target, std::wstring_view value) {
    switch (target) {
        case TargetKind::Id: return DeviceSelector::ById(value);
        case TargetKind::Name: return DeviceSelector::ByQuery(DeviceSelectorKind::Name, value);
        case TargetKind::Mac: return DeviceSelector::ByQuery(DeviceSelectorKind::Mac, value);
        case TargetKind::Auto: return DeviceSelector::ByQuery(DeviceSelectorKind::Auto, value);
        case TargetKind::Alias: return DeviceSelector::ByQuery(DeviceSelectorKind::Alias, value);
        case TargetKind::Last:
            if (value.empty()) return DeviceSelector::Last();
            return std::nullopt;
        case TargetKind::Default:
            if (value.empty()) return DeviceSelector::Default();
            return std::nullopt;
        case TargetKind::None: return std::nullopt;
    }
    return std::nullopt;
}

struct Translation {
    std::optional<AppCommand> Command;
    AppOutcomeReason Failure = AppOutcomeReason::Unsupported;
};

Translation Translate(Request const& request) {
    const auto noTarget = [&]() -> Translation {
        if (request.Target != TargetKind::None || !request.Payload.empty()) {
            return {.Failure = AppOutcomeReason::TargetRequired};
        }
        return {};
    };
    const auto explicitTarget = [&](TargetKind target, std::wstring_view value) -> Translation {
        auto selector = MakeSelector(target, value);
        if (!selector) return {.Failure = AppOutcomeReason::TargetRequired};
        return {.Command = AppCommand{AppCommandKind::Status, std::move(*selector), {}}};
    };

    switch (request.Command) {
        case CommandType::Show: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ShowDevicePicker, {}, {}};
            return result;
        }
        case CommandType::Settings: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ShowSettings, {}, {}};
            return result;
        }
        case CommandType::List: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ListDevices, {}, {}};
            return result;
        }
        case CommandType::Status: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::Status, {}, {}};
            return result;
        }
        case CommandType::DefaultShow: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ShowDefault, {}, {}};
            return result;
        }
        case CommandType::DefaultClear: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ClearDefault, {}, {}};
            return result;
        }
        case CommandType::AliasList: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ListAliases, {}, {}};
            return result;
        }
        case CommandType::DefaultSet: {
            auto result = explicitTarget(request.Target, request.Payload);
            if (!result.Command) return result;
            result.Command->Kind = AppCommandKind::SetDefault;
            return result;
        }
        case CommandType::AliasClear: {
            auto result = explicitTarget(request.Target, request.Payload);
            if (!result.Command) return result;
            result.Command->Kind = AppCommandKind::ClearAlias;
            return result;
        }
        case CommandType::Connect:
        case CommandType::Disconnect:
        case CommandType::Reconnect:
        case CommandType::ToggleLast: {
            auto result = explicitTarget(request.Target, request.Payload);
            if (!result.Command) return result;
            result.Command->Kind = request.Command == CommandType::Connect      ? AppCommandKind::Connect
                                   : request.Command == CommandType::Disconnect ? AppCommandKind::Disconnect
                                   : request.Command == CommandType::Reconnect  ? AppCommandKind::Reconnect
                                                                                : AppCommandKind::ToggleLast;
            return result;
        }
        case CommandType::DisconnectAll: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::DisconnectAll, {}, {}};
            return result;
        }
        case CommandType::ReconnectAll: {
            auto result = noTarget();
            if (!result.Command && result.Failure != AppOutcomeReason::Unsupported) return result;
            result.Command = AppCommand{AppCommandKind::ReconnectAll, {}, {}};
            return result;
        }
        case CommandType::AliasSet: {
            const auto separator = request.Payload.find(L'\n');
            if (separator == std::wstring::npos || separator == 0 || separator + 1 >= request.Payload.size()) {
                return {.Failure = AppOutcomeReason::InvalidAliasPayload};
            }
            const auto targetText = request.Payload.substr(0, separator);
            const auto alias = request.Payload.substr(separator + 1);
            auto selector = MakeSelector(request.Target, targetText);
            if (!selector) return {.Failure = AppOutcomeReason::TargetRequired};
            return {.Command = AppCommand{AppCommandKind::SetAlias, std::move(*selector), std::move(alias)}};
        }
        case CommandType::Unknown: return {.Failure = AppOutcomeReason::Unsupported};
    }
    return {.Failure = AppOutcomeReason::Unsupported};
}

bool IsMutating(CommandType command) noexcept {
    switch (command) {
        case CommandType::Connect:
        case CommandType::Disconnect:
        case CommandType::Reconnect:
        case CommandType::ToggleLast:
        case CommandType::DisconnectAll:
        case CommandType::ReconnectAll:
        case CommandType::DefaultSet:
        case CommandType::DefaultClear:
        case CommandType::AliasSet:
        case CommandType::AliasClear: return true;
        default: return false;
    }
}

bool IsQuery(AppCommandKind command) noexcept {
    switch (command) {
        case AppCommandKind::ListDevices:
        case AppCommandKind::Status:
        case AppCommandKind::ShowDefault:
        case AppCommandKind::ListAliases: return true;
        default: return false;
    }
}

ExitCode ToExitCode(AppResultCode code) noexcept {
    switch (code) {
        case AppResultCode::Success: return ExitCode::Success;
        case AppResultCode::InvalidInput: return ExitCode::InvalidRequest;
        case AppResultCode::NotFound: return ExitCode::NotFound;
        case AppResultCode::Ambiguous: return ExitCode::Ambiguous;
        case AppResultCode::OperationFailed: return ExitCode::OperationFailed;
        case AppResultCode::Unavailable: return ExitCode::Unavailable;
        case AppResultCode::Busy: return ExitCode::Busy;
        case AppResultCode::Cancelled:
        case AppResultCode::TimedOut:
        case AppResultCode::Indeterminate:
        case AppResultCode::InternalError: return ExitCode::Indeterminate;
    }
    return ExitCode::Indeterminate;
}

std::wstring LowerInvariant(std::wstring_view value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (const auto character : value)
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    return lowered;
}

std::wstring DeviceIdText(apc::app::DeviceSnapshot const& device) {
    return device.Id.ToString();
}

std::wstring DeviceDisplayName(ControlCommandAdapter::Localize const& localize,
                               apc::app::DeviceSnapshot const& device,
                               bool redact) {
    if (redact) return Resource(localize, "Privacy_RedactedDevice");
    if (!device.DisplayName.empty()) return device.DisplayName;
    if (!device.Alias.empty()) return device.Alias;
    if (!device.Name.empty()) return device.Name;
    return DeviceIdText(device);
}

std::wstring TargetDisplayName(ControlCommandAdapter::Localize const& localize,
                               apc::app::AppTargetSnapshot const& target,
                               bool redact) {
    if (redact) return Resource(localize, "Privacy_RedactedDevice");
    if (!target.DisplayName.empty()) return target.DisplayName;
    if (!target.Alias.empty()) return target.Alias;
    if (!target.Name.empty()) return target.Name;
    return target.Id;
}

std::wstring ResponseId(ControlCommandAdapter::Localize const& localize, std::wstring_view id, bool redact) {
    if (redact && !id.empty()) return Resource(localize, "Privacy_RedactedValue");
    return std::wstring(id);
}

std::wstring ResponseName(apc::app::DeviceSnapshot const& device, bool redact) {
    return redact ? std::wstring{} : device.Name;
}

std::vector<apc::app::DeviceSnapshot> SortedDevices(std::vector<apc::app::DeviceSnapshot> devices) {
    std::unordered_map<std::wstring, std::size_t> indexes;
    std::vector<apc::app::DeviceSnapshot> merged;
    merged.reserve(devices.size());
    for (auto& device : devices) {
        const auto id = DeviceIdText(device);
        const auto [entry, inserted] = indexes.emplace(id, merged.size());
        if (inserted) {
            merged.push_back(std::move(device));
            continue;
        }

        auto& existing = merged[entry->second];
        if (existing.Name.empty()) existing.Name = std::move(device.Name);
        if (existing.Alias.empty()) existing.Alias = std::move(device.Alias);
        if (existing.DisplayName.empty()) existing.DisplayName = std::move(device.DisplayName);
        existing.IsConnected = existing.IsConnected || device.IsConnected;
        existing.IsKnown = existing.IsKnown || device.IsKnown;
        existing.IsBusy = existing.IsBusy || device.IsBusy;
        if (existing.State == apc::app::DeviceConnectionState::Idle &&
            device.State != apc::app::DeviceConnectionState::Idle) {
            existing.State = device.State;
        }
    }

    std::ranges::sort(merged, [](auto const& left, auto const& right) {
        const auto leftLabel =
            LowerInvariant(left.Alias.empty() ? (left.Name.empty() ? left.Id.View() : left.Name) : left.Alias);
        const auto rightLabel =
            LowerInvariant(right.Alias.empty() ? (right.Name.empty() ? right.Id.View() : right.Name) : right.Alias);
        if (leftLabel != rightLabel) return leftLabel < rightLabel;
        return LowerInvariant(left.Id.View()) < LowerInvariant(right.Id.View());
    });
    return merged;
}

void InsertDeviceJson(JsonObject const& object,
                      ControlCommandAdapter::Localize const& localize,
                      apc::app::DeviceSnapshot const& device,
                      bool redact) {
    object.Insert(L"id",
                  JsonValue::CreateStringValue(winrt::hstring(ResponseId(localize, DeviceIdText(device), redact))));
    object.Insert(L"name", JsonValue::CreateStringValue(winrt::hstring(ResponseName(device, redact))));
    object.Insert(L"alias", JsonValue::CreateStringValue(winrt::hstring(device.Alias)));
    object.Insert(L"displayName",
                  JsonValue::CreateStringValue(winrt::hstring(DeviceDisplayName(localize, device, redact))));
    object.Insert(L"connected", JsonValue::CreateBooleanValue(device.IsConnected));
    object.Insert(L"known", JsonValue::CreateBooleanValue(device.IsKnown));
    object.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redact));
}

std::wstring ActionName(AppCommandKind command) {
    switch (command) {
        case AppCommandKind::ShowDevicePicker: return L"show";
        case AppCommandKind::ShowSettings: return L"settings";
        case AppCommandKind::SetDefault: return L"default-set";
        case AppCommandKind::ClearDefault: return L"default-clear";
        case AppCommandKind::SetAlias: return L"alias-set";
        case AppCommandKind::ClearAlias: return L"alias-clear";
        case AppCommandKind::Connect: return L"connect";
        case AppCommandKind::Disconnect: return L"disconnect";
        case AppCommandKind::Reconnect: return L"reconnect";
        case AppCommandKind::ToggleLast: return L"toggle";
        case AppCommandKind::DisconnectAll: return L"disconnect-all";
        case AppCommandKind::ReconnectAll: return L"reconnect-all";
        default: return {};
    }
}

std::wstring CommandTargetText(AppCommand const& command) {
    if (!command.Target) return {};
    if (command.Target->Kind() == DeviceSelectorKind::Id) return std::wstring(command.Target->IdText());
    if (command.Target->Kind() == DeviceSelectorKind::Last || command.Target->Kind() == DeviceSelectorKind::Default) {
        return {};
    }
    return std::wstring(command.Target->Query());
}

std::wstring FailureMessage(ControlCommandAdapter::Localize const& localize,
                            AppResult const& result,
                            AppCommand const& command,
                            bool redact) {
    const auto rawTarget = result.RequestedTarget.empty() ? CommandTargetText(command) : result.RequestedTarget;
    const auto target = result.Target   ? TargetDisplayName(localize, *result.Target, redact)
                        : result.Device ? DeviceDisplayName(localize, *result.Device, redact)
                                        : rawTarget;
    switch (result.Reason) {
        case AppOutcomeReason::NotReady: return Resource(localize, "Command_NotReady");
        case AppOutcomeReason::TargetRequired: return Resource(localize, "Command_TargetRequired");
        case AppOutcomeReason::TargetNotFound: return FormatResource(localize, "Command_TargetNotFound", rawTarget);
        case AppOutcomeReason::TargetAmbiguous: return FormatResource(localize, "Command_TargetAmbiguous", rawTarget);
        case AppOutcomeReason::DefaultTargetMissing: return Resource(localize, "Command_DefaultTargetMissing");
        case AppOutcomeReason::LastTargetMissing: return Resource(localize, "Command_LastTargetMissing");
        case AppOutcomeReason::InvalidAliasPayload: return Resource(localize, "Command_InvalidAliasPayload");
        case AppOutcomeReason::Unsupported: return Resource(localize, "Command_Unsupported");
        default: break;
    }

    if (result.Code == AppResultCode::NotFound) return FormatResource(localize, "Command_TargetNotFound", rawTarget);
    if (result.Code == AppResultCode::Ambiguous) return FormatResource(localize, "Command_TargetAmbiguous", rawTarget);
    if (result.Code == AppResultCode::Unavailable || result.Code == AppResultCode::Busy ||
        result.Code == AppResultCode::Cancelled) {
        return result.Code == AppResultCode::Busy ? Resource(localize, "Command_Busy")
                                                  : Resource(localize, "Command_NotReady");
    }

    switch (command.Kind) {
        case AppCommandKind::Connect: return FormatResource(localize, "Command_ConnectFailed", target);
        case AppCommandKind::ToggleLast:
            return result.Reason == AppOutcomeReason::DisconnectFailed
                       ? FormatResource(localize, "Command_DisconnectFailed", target)
                       : FormatResource(localize, "Command_ConnectFailed", target);
        case AppCommandKind::Disconnect: return FormatResource(localize, "Command_DisconnectFailed", target);
        case AppCommandKind::Reconnect: return FormatResource(localize, "Command_ReconnectFailed", target);
        case AppCommandKind::ReconnectAll: return FormatResource(localize, "Command_ReconnectFailed", target);
        case AppCommandKind::SetAlias: return FormatResource(localize, "Command_AliasSetFailed", target);
        case AppCommandKind::ClearAlias: return FormatResource(localize, "Command_AliasClearFailed", target);
        default: return Resource(localize, "Command_NotReady");
    }
}

std::wstring SuccessMessage(ControlCommandAdapter::Localize const& localize,
                            AppResult const& result,
                            AppCommand const& command,
                            std::wstring_view displayName) {
    switch (result.Reason) {
        case AppOutcomeReason::AlreadyConnected:
            return FormatResource(localize, "Command_DeviceAlreadyConnected", displayName);
        case AppOutcomeReason::AlreadyDisconnected:
            return FormatResource(localize, "Command_DeviceAlreadyDisconnected", displayName);
        case AppOutcomeReason::ConnectSucceeded:
            return FormatResource(localize, "Command_ConnectSucceeded", displayName);
        case AppOutcomeReason::DisconnectSucceeded:
            return FormatResource(localize, "Command_DisconnectSucceeded", displayName);
        case AppOutcomeReason::ReconnectSucceeded:
            return FormatResource(localize, "Command_ReconnectSucceeded", displayName);
        case AppOutcomeReason::ShowOpened: return Resource(localize, "Command_ShowOpened");
        case AppOutcomeReason::SettingsOpened: return Resource(localize, "Command_SettingsOpened");
        case AppOutcomeReason::DefaultSet: return FormatResource(localize, "Command_DefaultSet", displayName);
        case AppOutcomeReason::DefaultCleared: return Resource(localize, "Command_DefaultCleared");
        case AppOutcomeReason::AliasSet: return FormatResource(localize, "Command_AliasSet", displayName, result.Alias);
        case AppOutcomeReason::AliasCleared: return FormatResource(localize, "Command_AliasCleared", displayName);
        case AppOutcomeReason::DisconnectAllSucceeded: return Resource(localize, "Command_DisconnectAllSucceeded");
        case AppOutcomeReason::ReconnectAllSucceeded: return Resource(localize, "Command_ReconnectAllSucceeded");
        default: break;
    }

    switch (command.Kind) {
        case AppCommandKind::ShowDevicePicker: return Resource(localize, "Command_ShowOpened");
        case AppCommandKind::ShowSettings: return Resource(localize, "Command_SettingsOpened");
        case AppCommandKind::SetDefault: return FormatResource(localize, "Command_DefaultSet", displayName);
        case AppCommandKind::ClearDefault: return Resource(localize, "Command_DefaultCleared");
        case AppCommandKind::SetAlias:
            return FormatResource(
                localize, "Command_AliasSet", displayName, result.Alias.empty() ? command.Alias : result.Alias);
        case AppCommandKind::ClearAlias: return FormatResource(localize, "Command_AliasCleared", displayName);
        case AppCommandKind::DisconnectAll: return Resource(localize, "Command_DisconnectAllSucceeded");
        case AppCommandKind::ReconnectAll: return Resource(localize, "Command_ReconnectAllSucceeded");
        default: return {};
    }
}

Response MessageResponse(Request const& request, ExitCode code, std::wstring message, bool wantsJson) {
    Response response{code, {}, request.CorrelationId};
    if (!wantsJson) {
        response.Payload = std::move(message);
        return response;
    }

    JsonObject root;
    root.Insert(L"ok", JsonValue::CreateBooleanValue(code == ExitCode::Success));
    root.Insert(L"exitCode", JsonValue::CreateNumberValue(static_cast<double>(code)));
    root.Insert(L"message", JsonValue::CreateStringValue(winrt::hstring(message)));
    response.Payload = std::wstring(root.Stringify());
    return response;
}

Response OperationResponse(Request const& request,
                           ExitCode code,
                           std::wstring_view action,
                           std::wstring_view id,
                           std::wstring_view name,
                           bool redact,
                           std::wstring message,
                           ControlCommandAdapter::Localize const& localize,
                           bool wantsJson) {
    Response response{code, {}, request.CorrelationId};
    if (!wantsJson) {
        response.Payload = std::move(message);
        return response;
    }

    JsonObject root;
    root.Insert(L"ok", JsonValue::CreateBooleanValue(code == ExitCode::Success));
    root.Insert(L"exitCode", JsonValue::CreateNumberValue(static_cast<double>(code)));
    root.Insert(L"action", JsonValue::CreateStringValue(winrt::hstring(action)));
    root.Insert(L"id", JsonValue::CreateStringValue(winrt::hstring(ResponseId(localize, id, redact))));
    root.Insert(L"name", JsonValue::CreateStringValue(winrt::hstring(name)));
    root.Insert(L"displayName", JsonValue::CreateStringValue(winrt::hstring(name)));
    root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redact));
    root.Insert(L"message", JsonValue::CreateStringValue(winrt::hstring(message)));
    response.Payload = std::wstring(root.Stringify());
    return response;
}

AppSnapshot SnapshotFor(AppResult const& result, AppSnapshot&& fallback) {
    if (result.Snapshot) return *result.Snapshot;
    return std::move(fallback);
}

std::vector<apc::app::DeviceSnapshot> DevicesFor(AppResult const& result, AppSnapshot const& snapshot) {
    if (!result.Devices.empty()) return result.Devices;
    return snapshot.Devices;
}

void InsertResourceStatusJson(JsonObject const& root, AppSnapshot::ResourceStatusSnapshot const& status) {
    const auto residencyName = [](AppSnapshot::ResourceStatusSnapshot::Residency value) -> std::wstring_view {
        switch (value) {
            case AppSnapshot::ResourceStatusSnapshot::Residency::Cold: return L"Cold";
            case AppSnapshot::ResourceStatusSnapshot::Residency::Warm: return L"Warm";
            case AppSnapshot::ResourceStatusSnapshot::Residency::Hot: return L"Hot";
        }
        return L"Unknown";
    };
    const auto memoryName = [](AppSnapshot::ResourceStatusSnapshot::MemoryPressure value) -> std::wstring_view {
        switch (value) {
            case AppSnapshot::ResourceStatusSnapshot::MemoryPressure::Unknown: return L"Unknown";
            case AppSnapshot::ResourceStatusSnapshot::MemoryPressure::Low: return L"Low";
            case AppSnapshot::ResourceStatusSnapshot::MemoryPressure::Neutral: return L"Neutral";
            case AppSnapshot::ResourceStatusSnapshot::MemoryPressure::High: return L"High";
        }
        return L"Unknown";
    };
    const auto activityName = [](AppSnapshot::ResourceStatusSnapshot::UserActivity value) -> std::wstring_view {
        switch (value) {
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::Unknown: return L"Unknown";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::Available: return L"Available";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::NotPresent: return L"NotPresent";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::Busy: return L"Busy";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::Fullscreen: return L"Fullscreen";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::Presentation: return L"Presentation";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::QuietTime: return L"QuietTime";
            case AppSnapshot::ResourceStatusSnapshot::UserActivity::ImmersiveApp: return L"ImmersiveApp";
        }
        return L"Unknown";
    };

    JsonObject adaptiveResources;
    adaptiveResources.Insert(L"evaluated", JsonValue::CreateBooleanValue(status.Evaluated));
    adaptiveResources.Insert(L"residency",
                             JsonValue::CreateStringValue(winrt::hstring(residencyName(status.ForegroundResidency))));
    adaptiveResources.Insert(L"backgroundResidency",
                             JsonValue::CreateStringValue(winrt::hstring(residencyName(status.BackgroundResidency))));
    adaptiveResources.Insert(L"snapshotFresh", JsonValue::CreateBooleanValue(status.SnapshotFresh));
    adaptiveResources.Insert(L"positiveAuthorizationCurrent",
                             JsonValue::CreateBooleanValue(status.PositiveAuthorizationCurrent));
    adaptiveResources.Insert(L"preloadAllowed", JsonValue::CreateBooleanValue(status.PreloadAllowed));
    adaptiveResources.Insert(L"uiResourcesLoaded", JsonValue::CreateBooleanValue(status.UiResourcesLoaded));
    adaptiveResources.Insert(L"uiResourcesInitialized", JsonValue::CreateBooleanValue(status.UiResourcesInitialized));
    adaptiveResources.Insert(L"memoryPressure",
                             JsonValue::CreateStringValue(winrt::hstring(memoryName(status.Memory))));
    adaptiveResources.Insert(L"userActivity",
                             JsonValue::CreateStringValue(winrt::hstring(activityName(status.Activity))));
    adaptiveResources.Insert(L"energySaver",
                             status.EnergySaver ? JsonValue::CreateBooleanValue(*status.EnergySaver)
                                                : JsonValue::CreateNullValue());
    root.Insert(L"adaptiveResources", adaptiveResources);
}

Response FormatQuery(Request const& request,
                     AppResult const& result,
                     AppSnapshot snapshot,
                     ControlCommandAdapter::Localize const& localize,
                     bool redact,
                     bool wantsJson) {
    const auto code = ToExitCode(result.Code);
    if (result.Code != AppResultCode::Success) {
        if (result.Code == AppResultCode::InternalError) return {code, {}, request.CorrelationId};
        return MessageResponse(
            request, code, FailureMessage(localize, result, AppCommand{result.Command, {}, {}}, redact), wantsJson);
    }

    const auto devices = SortedDevices(DevicesFor(result, snapshot));
    switch (result.Command) {
        case AppCommandKind::ListDevices: {
            if (wantsJson) {
                JsonObject root;
                JsonArray array;
                for (auto const& device : devices) {
                    JsonObject object;
                    InsertDeviceJson(object, localize, device, redact);
                    array.Append(object);
                }
                root.Insert(L"devices", array);
                return {ExitCode::Success, std::wstring(root.Stringify()), request.CorrelationId};
            }
            if (devices.empty()) {
                return {ExitCode::Success, Resource(localize, "Command_List_NoDevices") + L"\n", request.CorrelationId};
            }
            std::wstringstream output;
            output << Resource(localize, "Command_List_Header") << L"\n";
            for (auto const& device : devices) {
                output << L"- " << DeviceDisplayName(localize, device, redact);
                if (device.IsConnected) output << L" (" << Resource(localize, "Command_ConnectedSuffix") << L")";
                output << L"\n  ID: " << ResponseId(localize, DeviceIdText(device), redact) << L"\n";
            }
            return {ExitCode::Success, output.str(), request.CorrelationId};
        }
        case AppCommandKind::Status: {
            std::vector<apc::app::DeviceSnapshot> connected;
            std::ranges::copy_if(
                devices, std::back_inserter(connected), [](auto const& device) { return device.IsConnected; });
            if (wantsJson) {
                JsonObject root;
                JsonArray array;
                for (auto const& device : connected) {
                    JsonObject object;
                    InsertDeviceJson(object, localize, device, redact);
                    array.Append(object);
                }
                root.Insert(L"running", JsonValue::CreateBooleanValue(true));
                root.Insert(L"connectedCount", JsonValue::CreateNumberValue(static_cast<double>(connected.size())));
                root.Insert(L"connectedDevices", array);
                root.Insert(
                    L"devicePickerOpenedGeneration",
                    JsonValue::CreateNumberValue(static_cast<double>(snapshot.Tray.DevicePickerOpenedGeneration)));
                InsertResourceStatusJson(root, snapshot.AdaptiveResources);
                return {ExitCode::Success, std::wstring(root.Stringify()), request.CorrelationId};
            }
            std::wstringstream output;
            output << Resource(localize, "Command_Status_Running") << L"\n";
            output << FormatResource(localize, "Command_Status_Connections", connected.size()) << L"\n";
            for (auto const& device : connected) {
                output << L"- " << DeviceDisplayName(localize, device, redact) << L"\n  ID: "
                       << ResponseId(localize, DeviceIdText(device), redact) << L"\n";
            }
            return {ExitCode::Success, output.str(), request.CorrelationId};
        }
        case AppCommandKind::ShowDefault: {
            auto defaultDevice = result.DefaultDevice ? result.DefaultDevice : snapshot.DefaultDevice;
            const auto mode = defaultDevice && defaultDevice->Mode == apc::app::DefaultDeviceMode::SpecificDevice
                                  ? L"specificDevice"
                                  : L"lastConnected";
            std::optional<apc::app::DeviceSnapshot> target;
            if (defaultDevice && defaultDevice->Id) {
                const auto id = defaultDevice->Id->View();
                auto found = std::ranges::find_if(devices, [id](auto const& device) { return device.Id.View() == id; });
                if (found != devices.end()) {
                    target = *found;
                } else {
                    auto synthetic = apc::app::DeviceSnapshot{*defaultDevice->Id,
                                                              {},
                                                              {},
                                                              defaultDevice->DisplayName,
                                                              apc::app::DeviceConnectionState::Idle,
                                                              defaultDevice->IsResolved,
                                                              defaultDevice->IsConnected,
                                                              false};
                    target = std::move(synthetic);
                }
            } else if (!snapshot.LastConnectedDeviceIds.empty()) {
                const auto id = snapshot.LastConnectedDeviceIds.front();
                auto found =
                    std::ranges::find_if(devices, [&id](auto const& device) { return device.Id.View() == id.View(); });
                if (found != devices.end()) {
                    target = *found;
                } else {
                    target = apc::app::DeviceSnapshot{
                        id, {}, {}, {}, apc::app::DeviceConnectionState::Idle, false, false, false};
                }
            }

            const auto displayName = target ? DeviceDisplayName(localize, *target, redact) : std::wstring{};
            if (wantsJson) {
                JsonObject root;
                root.Insert(L"ok", JsonValue::CreateBooleanValue(true));
                root.Insert(L"mode", JsonValue::CreateStringValue(winrt::hstring(mode)));
                root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redact));
                root.Insert(L"id",
                            JsonValue::CreateStringValue(winrt::hstring(
                                target ? ResponseId(localize, DeviceIdText(*target), redact) : std::wstring{})));
                root.Insert(L"displayName", JsonValue::CreateStringValue(winrt::hstring(displayName)));
                root.Insert(L"resolved", JsonValue::CreateBooleanValue(target && target->IsKnown));
                root.Insert(L"connected", JsonValue::CreateBooleanValue(target && target->IsConnected));
                return {ExitCode::Success, std::wstring(root.Stringify()), request.CorrelationId};
            }
            if (!target)
                return {ExitCode::Success,
                        Resource(localize, "Command_DefaultMode_LastConnected") + L"\n",
                        request.CorrelationId};
            return {ExitCode::Success,
                    FormatResource(localize, "Command_DefaultMode_Specific", displayName) + L"\n",
                    request.CorrelationId};
        }
        case AppCommandKind::ListAliases: {
            if (wantsJson) {
                JsonObject root;
                JsonArray array;
                for (auto const& device : devices) {
                    JsonObject object;
                    InsertDeviceJson(object, localize, device, redact);
                    object.Insert(L"hasAlias", JsonValue::CreateBooleanValue(!device.Alias.empty()));
                    array.Append(object);
                }
                root.Insert(L"devices", array);
                root.Insert(L"privacyRedacted", JsonValue::CreateBooleanValue(redact));
                return {ExitCode::Success, std::wstring(root.Stringify()), request.CorrelationId};
            }
            if (devices.empty()) {
                return {ExitCode::Success,
                        Resource(localize, "Command_AliasList_NoDevices") + L"\n",
                        request.CorrelationId};
            }
            std::wstringstream output;
            output << Resource(localize, "Command_AliasList_Header") << L"\n";
            for (auto const& device : devices) {
                output << L"- " << DeviceDisplayName(localize, device, redact) << L": "
                       << (device.Alias.empty() ? Resource(localize, "Command_AliasNone") : device.Alias) << L"\n";
                output << L"  ID: " << ResponseId(localize, DeviceIdText(device), redact) << L"\n";
            }
            return {ExitCode::Success, output.str(), request.CorrelationId};
        }
        default:
            return MessageResponse(
                request, ExitCode::InvalidRequest, Resource(localize, "Command_Unsupported"), wantsJson);
    }
}

Response FormatOperation(Request const& request,
                         AppCommand const& command,
                         AppResult const& result,
                         ControlCommandAdapter::Localize const& localize,
                         bool redact,
                         bool wantsJson) {
    const auto code = ToExitCode(result.Code);
    if (result.Code == AppResultCode::InternalError) return {code, {}, request.CorrelationId};
    const auto hasResolvedTarget = [&]() noexcept { return result.Device || (result.Target && result.Target->Exists); };
    const auto isResolutionFailure = [&]() noexcept {
        switch (result.Reason) {
            case AppOutcomeReason::TargetRequired:
            case AppOutcomeReason::TargetNotFound:
            case AppOutcomeReason::TargetAmbiguous:
            case AppOutcomeReason::DefaultTargetMissing:
            case AppOutcomeReason::LastTargetMissing:
            case AppOutcomeReason::InvalidAliasPayload:
            case AppOutcomeReason::Unsupported: return !hasResolvedTarget();
            default:
                return !hasResolvedTarget() &&
                       (result.Code == AppResultCode::InvalidInput || result.Code == AppResultCode::NotFound ||
                        result.Code == AppResultCode::Ambiguous);
        }
    }();
    if (result.Code != AppResultCode::Success && isResolutionFailure) {
        return MessageResponse(request, code, FailureMessage(localize, result, command, redact), wantsJson);
    }

    const auto device = result.Device;
    std::wstring id = result.Target ? result.Target->Id : device ? DeviceIdText(*device) : std::wstring{};
    std::wstring displayName = result.Target ? TargetDisplayName(localize, *result.Target, redact)
                               : device      ? DeviceDisplayName(localize, *device, redact)
                                             : std::wstring{};
    if (!result.Target && !device && command.Target && command.Target->Kind() == DeviceSelectorKind::Id) {
        id = std::wstring(command.Target->IdText());
        displayName = redact ? Resource(localize, "Privacy_RedactedDevice") : id;
    }

    std::wstring jsonName = displayName;
    if (command.Kind == AppCommandKind::SetAlias && code == ExitCode::Success) {
        if (device) {
            jsonName = DeviceDisplayName(localize, *device, redact);
        } else if (!result.Alias.empty()) {
            jsonName = redact ? Resource(localize, "Privacy_RedactedDevice") : result.Alias;
        }
    }
    if (command.Kind == AppCommandKind::ClearAlias && code == ExitCode::Success && (device || result.Target)) {
        jsonName = redact ? Resource(localize, "Privacy_RedactedDevice")
                          : (result.Target ? (result.Target->Name.empty() ? id : result.Target->Name)
                                           : (device->Name.empty() ? id : device->Name));
    }

    auto message = result.Code == AppResultCode::Success ? SuccessMessage(localize, result, command, displayName)
                                                         : FailureMessage(localize, result, command, redact);
    if (result.Code == AppResultCode::Success && message.empty() && command.Kind == AppCommandKind::ToggleLast) {
        message = Resource(localize, "Command_NotReady");
    }
    if (result.Code == AppResultCode::TimedOut && result.Reason == AppOutcomeReason::None) {
        message = command.Kind == AppCommandKind::Reconnect
                      ? FormatResource(localize, "Command_ReconnectFailed", displayName)
                      : FormatResource(localize, "Command_ConnectFailed", displayName);
    }

    return OperationResponse(
        request, code, ActionName(command.Kind), id, jsonName, redact, std::move(message), localize, wantsJson);
}

AppCommandContext MakeContext(std::stop_token stopToken, std::uint64_t deadline) {
    AppCommandContext context;
    context.StopToken = stopToken;
    context.Completion = AppCommandContext::CompletionMode::WaitForCompletion;
    if (deadline == 0) return context;

    const auto nowTick = GetTickCount64();
    const auto now = AppCommandContext::Clock::now();
    if (deadline <= nowTick) {
        context.Deadline = now;
        return context;
    }

    const auto remaining = deadline - nowTick;
    const auto maxMilliseconds = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());
    context.Deadline = now + std::chrono::milliseconds(static_cast<std::int64_t>(std::min(remaining, maxMilliseconds)));
    return context;
}

} // namespace

namespace apc::control {

ControlCommandAdapter::ControlCommandAdapter(apc::app::AppController const& controller, Options options)
    : m_controller(controller), m_options(std::move(options)) {}

Response ControlCommandAdapter::Handle(Request const& request,
                                       std::stop_token stopToken,
                                       std::uint64_t deadline) const noexcept {
    try {
        const bool wantsJson = (request.Flags & CommandFlagJson) != 0;
        const bool wantsRaw = (request.Flags & CommandFlagRaw) != 0;
        auto snapshot = m_controller.Snapshot();

        if (stopToken.stop_requested() || apc::control::RemainingWait(deadline) == 0 || !snapshot.IsRunning) {
            return MessageResponse(
                request, ExitCode::Unavailable, Resource(m_options.LocalizeResource, "Command_NotReady"), wantsJson);
        }
        if (!IsRequestValidForAdapter(request)) {
            return MessageResponse(request,
                                   ExitCode::InvalidRequest,
                                   Resource(m_options.LocalizeResource, "Command_Unsupported"),
                                   wantsJson);
        }

        const auto translation = Translate(request);
        if (!translation.Command) {
            const auto code = translation.Failure == AppOutcomeReason::InvalidAliasPayload ? ExitCode::InvalidRequest
                                                                                           : ExitCode::InvalidRequest;
            AppResult invalid;
            invalid.Code = AppResultCode::InvalidInput;
            invalid.Command = AppCommandKind::Status;
            invalid.Reason = translation.Failure;
            return MessageResponse(
                request, code, FailureMessage(m_options.LocalizeResource, invalid, AppCommand{}, false), wantsJson);
        }

        std::unique_lock mutationLock(m_mutationMutex, std::defer_lock);
        if (IsMutating(request.Command) && !mutationLock.try_lock()) {
            return MessageResponse(
                request, ExitCode::Busy, Resource(m_options.LocalizeResource, "Command_Busy"), wantsJson);
        }

        auto context = MakeContext(stopToken, deadline);
        auto result = m_controller.Execute(*translation.Command, context);
        auto currentSnapshot = SnapshotFor(result, std::move(snapshot));
        if (result.Tray) currentSnapshot.Tray = *result.Tray;
        const bool privacyMode = result.PrivacyModeEnabled.value_or(currentSnapshot.PrivacyModeEnabled);
        const bool redact = privacyMode && !wantsRaw;
        if (IsQuery(translation.Command->Kind)) {
            return FormatQuery(
                request, result, std::move(currentSnapshot), m_options.LocalizeResource, redact, wantsJson);
        }
        return FormatOperation(request, *translation.Command, result, m_options.LocalizeResource, redact, wantsJson);
    } catch (...) {
        return {ExitCode::Indeterminate, {}, request.CorrelationId};
    }
}

} // namespace apc::control
