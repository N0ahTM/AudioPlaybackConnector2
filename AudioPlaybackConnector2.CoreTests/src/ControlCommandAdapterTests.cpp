#include <control/ControlCommandAdapter.hpp>

#include <winrt/Windows.Data.Json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using apc::app::AppCommand;
using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppController;
using apc::app::AppOutcomeReason;
using apc::app::AppResult;
using apc::app::AppResultCode;
using apc::app::AppSnapshot;
using apc::app::DeviceConnectionState;
using apc::app::DeviceSelector;
using apc::control::CommandFlagJson;
using apc::control::CommandFlagRaw;
using apc::control::CommandType;
using apc::control::ControlCommandAdapter;
using apc::control::ExitCode;
using apc::control::Request;
using apc::control::Response;
using apc::control::TargetKind;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

std::wstring Localize(std::string_view key) {
    static const std::map<std::string, std::wstring> resources{
        {"Privacy_RedactedDevice", L"<device>"},
        {"Privacy_RedactedValue", L"<value>"},
        {"Command_NotReady", L"Not ready"},
        {"Command_Busy", L"Busy"},
        {"Command_Unsupported", L"Unsupported"},
        {"Command_TargetRequired", L"Target required"},
        {"Command_TargetNotFound", L"Target not found: {0}"},
        {"Command_TargetAmbiguous", L"Target ambiguous: {0}"},
        {"Command_DefaultTargetMissing", L"Default target missing"},
        {"Command_LastTargetMissing", L"Last target missing"},
        {"Command_InvalidAliasPayload", L"Invalid alias payload"},
        {"Command_List_Header", L"Devices"},
        {"Command_List_NoDevices", L"No devices"},
        {"Command_ConnectedSuffix", L"connected"},
        {"Command_Status_Running", L"Running"},
        {"Command_Status_Connections", L"Connections: {0}"},
        {"Command_DefaultMode_LastConnected", L"Mode: last connected"},
        {"Command_DefaultMode_Specific", L"Default: {0}"},
        {"Command_AliasList_Header", L"Aliases"},
        {"Command_AliasList_NoDevices", L"No alias devices"},
        {"Command_AliasNone", L"(none)"},
        {"Command_ShowOpened", L"Picker opened"},
        {"Command_SettingsOpened", L"Settings opened"},
        {"Command_DeviceAlreadyConnected", L"Already connected: {0}"},
        {"Command_DeviceAlreadyDisconnected", L"Already disconnected: {0}"},
        {"Command_ConnectSucceeded", L"Connected: {0}"},
        {"Command_ConnectFailed", L"Connect failed: {0}"},
        {"Command_DisconnectSucceeded", L"Disconnected: {0}"},
        {"Command_DisconnectFailed", L"Disconnect failed: {0}"},
        {"Command_ReconnectSucceeded", L"Reconnected: {0}"},
        {"Command_ReconnectFailed", L"Reconnect failed: {0}"},
        {"Command_DefaultSet", L"Default set: {0}"},
        {"Command_DefaultCleared", L"Default cleared"},
        {"Command_AliasSet", L"Alias {0} -> {1}"},
        {"Command_AliasSetFailed", L"Alias set failed: {0}"},
        {"Command_AliasCleared", L"Alias cleared: {0}"},
        {"Command_AliasClearFailed", L"Alias clear failed: {0}"},
        {"Command_DisconnectAllSucceeded", L"All disconnected"},
        {"Command_ReconnectAllSucceeded", L"All reconnected"},
    };
    if (auto found = resources.find(std::string(key)); found != resources.end()) return found->second;
    return std::wstring(key.begin(), key.end());
}

std::optional<apc::core::DeviceId> Id(std::wstring_view value) {
    return apc::core::DeviceId::TryCreate(value);
}

AppSnapshot FixtureSnapshot() {
    AppSnapshot snapshot;
    snapshot.Generation = 7;
    snapshot.IsRunning = true;
    snapshot.PrivacyModeEnabled = false;
    const auto headphones = Id(L"device-a");
    const auto speaker = Id(L"device-b");
    if (!headphones || !speaker) return snapshot;
    snapshot.Devices.push_back(
        {*headphones, L"Headphones", L"Desk", L"Desk", DeviceConnectionState::Connected, true, true, false});
    snapshot.Devices.push_back({*speaker, L"Speaker", {}, L"Speaker", DeviceConnectionState::Idle, true, false, false});
    snapshot.LastConnectedDeviceIds.push_back(*headphones);
    snapshot.DefaultDevice =
        apc::app::DefaultDeviceSnapshot{apc::app::DefaultDeviceMode::SpecificDevice, *headphones, L"Desk", true, true};
    snapshot.Tray.Generation = 7;
    snapshot.Tray.DevicePickerOpenedGeneration = 12;
    snapshot.Tray.ConnectedDevices.push_back(snapshot.Devices.front());
    snapshot.AdaptiveResources.Evaluated = true;
    snapshot.AdaptiveResources.ForegroundResidency = AppSnapshot::ResourceStatusSnapshot::Residency::Hot;
    snapshot.AdaptiveResources.BackgroundResidency = AppSnapshot::ResourceStatusSnapshot::Residency::Warm;
    snapshot.AdaptiveResources.SnapshotFresh = true;
    snapshot.AdaptiveResources.PositiveAuthorizationCurrent = true;
    snapshot.AdaptiveResources.PreloadAllowed = true;
    snapshot.AdaptiveResources.UiResourcesLoaded = true;
    snapshot.AdaptiveResources.UiResourcesInitialized = true;
    snapshot.AdaptiveResources.Memory = AppSnapshot::ResourceStatusSnapshot::MemoryPressure::Low;
    snapshot.AdaptiveResources.Activity = AppSnapshot::ResourceStatusSnapshot::UserActivity::Available;
    snapshot.AdaptiveResources.EnergySaver = false;
    return snapshot;
}

Request MakeRequest(CommandType command,
                    TargetKind target = TargetKind::None,
                    std::wstring payload = {},
                    std::uint32_t flags = 0) {
    Request request;
    request.Command = command;
    request.Target = target;
    request.Payload = std::move(payload);
    request.Flags = flags;
    request.CorrelationId = {0x11, 0x22};
    return request;
}

struct Harness {
    std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<AppCommand> Commands;
    std::vector<AppCommandContext> Contexts;
    AppResult Result;
    AppSnapshot Snapshot = FixtureSnapshot();
    AppController Controller;
    ControlCommandAdapter Adapter;

    Harness()
        : Controller(
              [this](AppCommand const& command, AppCommandContext const& context) {
                  {
                      std::lock_guard lock(Mutex);
                      Commands.push_back(command);
                      Contexts.push_back(context);
                  }
                  Changed.notify_all();
                  auto result = Result;
                  result.Command = command.Kind;
                  return result;
              },
              [this] {
                  std::lock_guard lock(Mutex);
                  return Snapshot;
              }),
          Adapter(Controller, ControlCommandAdapter::Options{Localize}) {
        Result.Code = AppResultCode::Success;
    }

    void ClearCommands() {
        std::lock_guard lock(Mutex);
        Commands.clear();
        Contexts.clear();
    }
};

void TestAllCommandMappingsAndTargets() {
    Harness harness;
    struct Case {
        Request RequestValue;
        AppCommandKind Kind;
    };
    const std::vector<Case> cases{
        {MakeRequest(CommandType::List), AppCommandKind::ListDevices},
        {MakeRequest(CommandType::Status), AppCommandKind::Status},
        {MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), AppCommandKind::Connect},
        {MakeRequest(CommandType::Disconnect, TargetKind::Name, L"Headphones"), AppCommandKind::Disconnect},
        {MakeRequest(CommandType::Reconnect, TargetKind::Mac, L"AABBCC"), AppCommandKind::Reconnect},
        {MakeRequest(CommandType::ToggleLast, TargetKind::Default), AppCommandKind::ToggleLast},
        {MakeRequest(CommandType::Connect, TargetKind::Last), AppCommandKind::Connect},
        {MakeRequest(CommandType::DisconnectAll), AppCommandKind::DisconnectAll},
        {MakeRequest(CommandType::ReconnectAll), AppCommandKind::ReconnectAll},
        {MakeRequest(CommandType::Show), AppCommandKind::ShowDevicePicker},
        {MakeRequest(CommandType::Settings), AppCommandKind::ShowSettings},
        {MakeRequest(CommandType::DefaultShow), AppCommandKind::ShowDefault},
        {MakeRequest(CommandType::DefaultSet, TargetKind::Auto, L"headphones"), AppCommandKind::SetDefault},
        {MakeRequest(CommandType::DefaultClear), AppCommandKind::ClearDefault},
        {MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a\nDesk"), AppCommandKind::SetAlias},
        {MakeRequest(CommandType::AliasClear, TargetKind::Alias, L"Desk"), AppCommandKind::ClearAlias},
        {MakeRequest(CommandType::AliasList), AppCommandKind::ListAliases},
    };

    for (auto const& test : cases) {
        harness.ClearCommands();
        const auto response = harness.Adapter.Handle(test.RequestValue, {}, apc::control::DeadlineAfter(1000));
        Check(response.Code == ExitCode::Success, "each valid control command must produce a success response");
        std::lock_guard lock(harness.Mutex);
        Check(harness.Commands.size() == 1, "each valid control command must call AppController once");
        if (harness.Commands.empty()) continue;
        const auto& command = harness.Commands.front();
        Check(command.Kind == test.Kind, "wire command must map to the matching typed application command");
        if (test.RequestValue.Command == CommandType::AliasSet) {
            Check(command.Alias == L"Desk" && command.Target && command.Target->IdText() == L"device-a",
                  "AliasSet must split target and alias without changing either value");
        }
        if (test.RequestValue.Command == CommandType::ToggleLast) {
            Check(command.Target && command.Target->Kind() == apc::app::DeviceSelectorKind::Default,
                  "CLI bare toggle normalization must arrive at the adapter as the Default selector");
        }
    }
}

void TestInvalidRequestsAndCompatibilityGrammar() {
    Harness harness;
    auto invalid = MakeRequest(CommandType::Status, TargetKind::Id, L"device-a");
    auto response = harness.Adapter.Handle(invalid, {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::InvalidRequest, "unexpected target shape must be rejected as InvalidRequest");
    Check(harness.Commands.empty(), "invalid control input must not reach AppController");

    harness.ClearCommands();
    auto alias = MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a");
    response = harness.Adapter.Handle(alias, {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::InvalidRequest && harness.Commands.empty(),
          "AliasSet without a target-newline-alias payload must be rejected");

    harness.ClearCommands();
    std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
    auto compatible = MakeRequest(CommandType::Connect, TargetKind::Name, invalidUtf16);
    response = harness.Adapter.Handle(compatible, {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success && harness.Commands.size() == 1,
          "bounded invalid UTF-16 accepted by IsRequestValid must remain transport-compatible at the model boundary");

    harness.ClearCommands();
    auto bareToggle = MakeRequest(CommandType::ToggleLast);
    response = harness.Adapter.Handle(bareToggle, {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::InvalidRequest && harness.Commands.empty(),
          "wire-level bare Toggle must remain invalid; the CLI parser supplies Default before transport");
}

void TestNonDeviceCommandsDoNotRequireInventorySnapshot() {
    std::size_t snapshotCalls = 0;
    std::size_t executeCalls = 0;
    AppController controller(
        [&](AppCommand const& command, AppCommandContext const&) {
            ++executeCalls;
            AppResult result;
            result.Code = AppResultCode::Success;
            result.Command = command.Kind;
            result.PrivacyModeEnabled = false;
            switch (command.Kind) {
                case AppCommandKind::ShowDevicePicker: result.Reason = AppOutcomeReason::ShowOpened; break;
                case AppCommandKind::ShowSettings: result.Reason = AppOutcomeReason::SettingsOpened; break;
                case AppCommandKind::ClearDefault: result.Reason = AppOutcomeReason::DefaultCleared; break;
                case AppCommandKind::DisconnectAll: result.Reason = AppOutcomeReason::DisconnectAllSucceeded; break;
                default: break;
            }
            return result;
        },
        [&]() -> AppSnapshot {
            ++snapshotCalls;
            throw std::runtime_error("device enumeration failed");
        });
    ControlCommandAdapter adapter(controller, ControlCommandAdapter::Options{Localize});

    const std::vector<Request> requests{MakeRequest(CommandType::Show),
                                        MakeRequest(CommandType::Settings),
                                        MakeRequest(CommandType::DefaultClear),
                                        MakeRequest(CommandType::DisconnectAll)};
    for (auto const& request : requests) {
        const auto response = adapter.Handle(request, {}, apc::control::DeadlineAfter(1000));
        Check(response.Code == ExitCode::Success,
              "a non-device control command must execute when the inventory snapshot read fails");
    }
    Check(executeCalls == requests.size(),
          "each non-device command must reach the shared AppController despite snapshot read failure");
    Check(snapshotCalls == 0,
          "non-device control commands must not obtain a device-enumerating snapshot for presentation");
}

void TestInventoryCommandsFailClosedOnSnapshotReadFailure() {
    std::size_t snapshotCalls = 0;
    std::size_t executeCalls = 0;
    AppController controller(
        [&](AppCommand const& command, AppCommandContext const&) {
            ++executeCalls;
            return AppResult{AppResultCode::Success, command.Kind};
        },
        [&]() -> AppSnapshot {
            ++snapshotCalls;
            throw std::runtime_error("device enumeration failed");
        });
    ControlCommandAdapter adapter(controller, ControlCommandAdapter::Options{Localize});

    const std::vector<Request> requests{MakeRequest(CommandType::List),
                                        MakeRequest(CommandType::Status),
                                        MakeRequest(CommandType::DefaultShow),
                                        MakeRequest(CommandType::AliasList)};
    for (auto const& request : requests) {
        const auto response = adapter.Handle(request, {}, apc::control::DeadlineAfter(1000));
        Check(response.Code == ExitCode::Unavailable && response.Payload == L"Not ready",
              "an inventory-backed control query must fail closed when its snapshot read fails");
    }
    Check(snapshotCalls == requests.size(), "each inventory-backed query must validate snapshot availability");
    Check(executeCalls == 0, "a failed inventory snapshot must prevent query dispatch against partial state");
}

void TestUiAndCliTypedParityAndContextPropagation() {
    Harness harness;
    auto target = DeviceSelector::ById(L"device-a");
    Check(target.has_value(), "parity fixture must have a valid device selector");
    if (!target) return;
    const AppCommand expected{AppCommandKind::Connect, *target, {}};

    std::stop_source stop;
    const auto response = harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"),
                                                 stop.get_token(),
                                                 apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success, "a valid CLI command must execute successfully");
    {
        std::lock_guard lock(harness.Mutex);
        Check(harness.Commands.size() == 1 && harness.Commands.front() == expected,
              "CLI mapping must produce the same typed command a UI caller would send");
        Check(harness.Contexts.size() == 1 && harness.Contexts.front().StopToken == stop.get_token(),
              "the adapter must preserve the control stop token in the typed context");
        Check(harness.Contexts.size() == 1 &&
                  harness.Contexts.front().Completion == AppCommandContext::CompletionMode::WaitForCompletion,
              "control commands must use the explicit wait-for-completion mode");
        Check(harness.Contexts.size() == 1 && harness.Contexts.front().Deadline != AppCommandContext::TimePoint::max(),
              "a finite control deadline must become a finite typed deadline");
    }

    harness.ClearCommands();
    stop.request_stop();
    auto cancelled = harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"),
                                            stop.get_token(),
                                            apc::control::DeadlineAfter(1000));
    Check(cancelled.Code == ExitCode::Unavailable && harness.Commands.empty(),
          "pre-dispatch control cancellation must preserve the legacy NotReady/Unavailable response");

    harness.ClearCommands();
    auto expired =
        harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), {}, GetTickCount64());
    Check(expired.Code == ExitCode::Unavailable && harness.Commands.empty(),
          "an expired absolute control deadline must stop before mutation dispatch");
}

void TestMutationBusyAndNonmutationConcurrency() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::vector<AppCommand> commands;
    AppSnapshot snapshot = FixtureSnapshot();
    AppController controller(
        [&](AppCommand const& command, AppCommandContext const&) {
            if (command.Kind == AppCommandKind::Connect) {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            }
            {
                std::lock_guard lock(mutex);
                commands.push_back(command);
            }
            return AppResult{AppResultCode::Success, command.Kind};
        },
        [&] { return snapshot; });
    ControlCommandAdapter adapter(controller, ControlCommandAdapter::Options{Localize});

    std::jthread first([&] {
        (void)adapter.Handle(
            MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), {}, apc::control::DeadlineAfter(5000));
    });
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }),
              "first mutation must enter the typed executor before the busy probe");
    }

    const auto busy = adapter.Handle(
        MakeRequest(CommandType::Disconnect, TargetKind::Id, L"device-a"), {}, apc::control::DeadlineAfter(1000));
    Check(busy.Code == ExitCode::Busy, "a concurrent mutation must receive the control Busy result");
    const auto status = adapter.Handle(MakeRequest(CommandType::Status), {}, apc::control::DeadlineAfter(1000));
    Check(status.Code == ExitCode::Success, "a nonmutation query must proceed while a mutation is in flight");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    first.join();
}

void TestResultExitMappingAndGoldenTextJsonPrivacy() {
    Harness harness;

    struct CodeCase {
        AppResultCode AppCode;
        ExitCode WireCode;
    };
    const std::vector<CodeCase> codes{{AppResultCode::Success, ExitCode::Success},
                                      {AppResultCode::InvalidInput, ExitCode::InvalidRequest},
                                      {AppResultCode::NotFound, ExitCode::NotFound},
                                      {AppResultCode::Ambiguous, ExitCode::Ambiguous},
                                      {AppResultCode::OperationFailed, ExitCode::OperationFailed},
                                      {AppResultCode::Unavailable, ExitCode::Unavailable},
                                      {AppResultCode::Busy, ExitCode::Busy},
                                      {AppResultCode::Cancelled, ExitCode::Indeterminate},
                                      {AppResultCode::TimedOut, ExitCode::Indeterminate},
                                      {AppResultCode::Indeterminate, ExitCode::Indeterminate},
                                      {AppResultCode::InternalError, ExitCode::Indeterminate}};
    for (auto const& code : codes) {
        harness.Result = {};
        harness.Result.Code = code.AppCode;
        harness.Result.Reason =
            code.AppCode == AppResultCode::OperationFailed ? AppOutcomeReason::ConnectFailed : AppOutcomeReason::None;
        if (code.AppCode == AppResultCode::InternalError) {
            auto result = harness.Adapter.Handle(
                MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), {}, apc::control::DeadlineAfter(1000));
            Check(result.Code == code.WireCode && result.Payload.empty(),
                  "internal controller errors must preserve the legacy empty indeterminate payload");
            continue;
        }
        auto result = harness.Adapter.Handle(
            MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), {}, apc::control::DeadlineAfter(1000));
        Check(result.Code == code.WireCode, "every normalized AppResultCode must map to its P01 exit code");
    }

    harness.Result = {};
    harness.Result.Code = AppResultCode::Success;
    auto listText = harness.Adapter.Handle(MakeRequest(CommandType::List), {}, apc::control::DeadlineAfter(1000));
    Check(listText.Payload == L"Devices\n- Desk (connected)\n  ID: device-a\n- Speaker\n  ID: device-b\n",
          "list text must preserve legacy ordering, connected suffix, and ID shape");

    auto listJson = harness.Adapter.Handle(
        MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagJson), {}, apc::control::DeadlineAfter(1000));
    Check(listJson.Payload == L"{\"devices\":[{\"id\":\"device-a\",\"name\":\"Headphones\",\"alias\":\"Desk\","
                              L"\"displayName\":\"Desk\",\"connected\":true,\"known\":true,\"privacyRedacted\":false},{"
                              L"\"id\":\"device-b\",\"name\":\"Speaker\",\"alias\":\"\",\"displayName\":\"Speaker\","
                              L"\"connected\":false,\"known\":true,\"privacyRedacted\":false}]}",
          "list JSON must preserve the established device object shape and field order");

    auto statusText = harness.Adapter.Handle(MakeRequest(CommandType::Status), {}, apc::control::DeadlineAfter(1000));
    Check(statusText.Payload == L"Running\nConnections: 1\n- Desk\n  ID: device-a\n",
          "status text must preserve the running/count/device presentation shape");

    auto statusJson = harness.Adapter.Handle(
        MakeRequest(CommandType::Status, TargetKind::None, {}, CommandFlagJson), {}, apc::control::DeadlineAfter(1000));
    Check(statusJson.Payload ==
              L"{\"running\":true,\"connectedCount\":1,\"connectedDevices\":[{\"id\":\"device-a\",\"name\":"
              L"\"Headphones\",\"alias\":\"Desk\",\"displayName\":\"Desk\",\"connected\":true,\"known\":true,"
              L"\"privacyRedacted\":false}],\"devicePickerOpenedGeneration\":12,\"adaptiveResources\":{\"evaluated\":"
              L"true,\"residency\":\"Hot\",\"backgroundResidency\":\"Warm\",\"snapshotFresh\":true,"
              L"\"positiveAuthorizationCurrent\":true,\"preloadAllowed\":true,\"uiResourcesLoaded\":true,"
              L"\"uiResourcesInitialized\":true,\"memoryPressure\":\"Low\",\"userActivity\":\"Available\","
              L"\"energySaver\":false}}",
          "status JSON must retain picker generation and complete adaptive-resource diagnostics");

    auto defaultJson =
        harness.Adapter.Handle(MakeRequest(CommandType::DefaultShow, TargetKind::None, {}, CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(defaultJson.Payload == L"{\"ok\":true,\"mode\":\"specificDevice\",\"privacyRedacted\":false,\"id\":\"device-"
                                 L"a\",\"displayName\":\"Desk\",\"resolved\":true,\"connected\":true}",
          "default JSON must retain mode, resolved, connected, and privacy fields");
    auto defaultText =
        harness.Adapter.Handle(MakeRequest(CommandType::DefaultShow), {}, apc::control::DeadlineAfter(1000));
    Check(defaultText.Payload == L"Default: Desk\n", "default text must preserve the localized specific-device shape");

    auto aliasJson = harness.Adapter.Handle(MakeRequest(CommandType::AliasList, TargetKind::None, {}, CommandFlagJson),
                                            {},
                                            apc::control::DeadlineAfter(1000));
    Check(aliasJson.Payload ==
              L"{\"devices\":[{\"id\":\"device-a\",\"name\":\"Headphones\",\"alias\":\"Desk\",\"displayName\":\"Desk\","
              L"\"connected\":true,\"known\":true,\"privacyRedacted\":false,\"hasAlias\":true},{\"id\":\"device-b\","
              L"\"name\":\"Speaker\",\"alias\":\"\",\"displayName\":\"Speaker\",\"connected\":false,\"known\":true,"
              L"\"privacyRedacted\":false,\"hasAlias\":false}],\"privacyRedacted\":false}",
          "alias-list JSON must retain per-device hasAlias and root privacy fields");
    auto aliasText = harness.Adapter.Handle(MakeRequest(CommandType::AliasList), {}, apc::control::DeadlineAfter(1000));
    Check(aliasText.Payload == L"Aliases\n- Desk: Desk\n  ID: device-a\n- Speaker: (none)\n  ID: device-b\n",
          "alias-list text must preserve alias labels, empty-alias text, and IDs");

    harness.Result = {};
    harness.Result.Code = AppResultCode::Success;
    harness.Result.Reason = AppOutcomeReason::ConnectSucceeded;
    harness.Result.Target = apc::app::AppTargetSnapshot{L"device-a", L"Headphones", L"Desk", L"Desk", true, true, true};
    auto connectText = harness.Adapter.Handle(
        MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"), {}, apc::control::DeadlineAfter(1000));
    Check(connectText.Payload == L"Connected: Desk",
          "connect operation text must preserve its localized action message");
    auto connectJson =
        harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a", CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(connectJson.Payload ==
              L"{\"ok\":true,\"exitCode\":0,\"action\":\"connect\",\"id\":\"device-a\",\"name\":\"Desk\","
              L"\"displayName\":\"Desk\",\"privacyRedacted\":false,\"message\":\"Connected: Desk\"}",
          "connect operation JSON must retain action, target, and privacy fields");

    harness.Result = {};
    harness.Result.Code = AppResultCode::Success;
    harness.Result.Reason = AppOutcomeReason::AliasSet;
    harness.Result.Alias = L"New Alias";
    harness.Result.Target = apc::app::AppTargetSnapshot{L"device-a", L"Headphones", L"Desk", L"Desk", true, true, true};
    harness.Result.Device = apc::app::DeviceSnapshot{*Id(L"device-a"),
                                                     L"Headphones",
                                                     L"New Alias",
                                                     L"New Alias",
                                                     DeviceConnectionState::Connected,
                                                     true,
                                                     true,
                                                     false};
    auto aliasOperation = harness.Adapter.Handle(
        MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a\nNew Alias", CommandFlagJson),
        {},
        apc::control::DeadlineAfter(1000));
    Check(aliasOperation.Payload.find(L"\"action\":\"alias-set\"") != std::wstring::npos &&
              aliasOperation.Payload.find(L"\"name\":\"New Alias\"") != std::wstring::npos &&
              aliasOperation.Payload.find(L"\"displayName\":\"New Alias\"") != std::wstring::npos,
          "AliasSet JSON must expose the post-change alias in name/displayName");
    Check(aliasOperation.Payload.find(L"Alias Desk -> New Alias") != std::wstring::npos,
          "AliasSet JSON message must use the localized pre-change label and new alias");

    harness.Result = {};
    harness.Result.Code = AppResultCode::Indeterminate;
    harness.Result.Reason = AppOutcomeReason::ReconnectFailed;
    harness.Result.Target =
        apc::app::AppTargetSnapshot{L"device-a", L"Headphones", L"Desk", L"Desk", true, false, true};
    auto reconnectAllFailure =
        harness.Adapter.Handle(MakeRequest(CommandType::ReconnectAll, TargetKind::None, {}, CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(reconnectAllFailure.Code == ExitCode::Indeterminate &&
              reconnectAllFailure.Payload.find(L"\"action\":\"reconnect-all\"") != std::wstring::npos &&
              reconnectAllFailure.Payload.find(L"Reconnect failed: Desk") != std::wstring::npos,
          "reconnect-all failures must preserve the operation action and localized target message");

    const std::wstring longId(600, L'x');
    harness.Result = {};
    harness.Result.Code = AppResultCode::NotFound;
    harness.Result.Reason = AppOutcomeReason::TargetNotFound;
    harness.Result.Target = apc::app::AppTargetSnapshot{longId, {}, {}, {}, false, false, false};
    harness.Result.RequestedTarget = longId;
    auto longIdFailure =
        harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, longId, CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(longIdFailure.Code == ExitCode::NotFound &&
              longIdFailure.Payload ==
                  L"{\"ok\":false,\"exitCode\":4,\"message\":\"Target not found: " + longId + L"\"}",
          "not-found formatting must preserve an unknown long ID without forcing a DeviceId snapshot");

    harness.Result = {};
    harness.Result.Code = AppResultCode::OperationFailed;
    harness.Result.Reason = AppOutcomeReason::ConnectFailed;
    harness.Result.Target =
        apc::app::AppTargetSnapshot{L"device-a", L"Headphones", L"Desk", L"Desk", true, false, true};
    harness.Result.PrivacyModeEnabled = true;
    auto privateFailure =
        harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a", CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(privateFailure.Payload.find(L"\"id\":\"<value>\"") != std::wstring::npos &&
              privateFailure.Payload.find(L"\"name\":\"<device>\"") != std::wstring::npos &&
              privateFailure.Payload.find(L"Connect failed: <device>") != std::wstring::npos,
          "known-target operation failures must redact ID, name, and message in privacy mode");

    harness.Result = {};
    harness.Result.Code = AppResultCode::NotFound;
    harness.Result.Reason = AppOutcomeReason::TargetNotFound;
    harness.Result.RequestedTarget = L"Headphones";
    harness.Result.PrivacyModeEnabled = true;
    auto privateQueryFailure =
        harness.Adapter.Handle(MakeRequest(CommandType::Connect, TargetKind::Name, L"Headphones", CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(privateQueryFailure.Payload.find(L"Target not found: Headphones") != std::wstring::npos,
          "not-found messages must preserve the current raw query compatibility quirk");

    harness.Snapshot.PrivacyModeEnabled = true;
    harness.Result = {};
    auto privateAliases =
        harness.Adapter.Handle(MakeRequest(CommandType::AliasList, TargetKind::None, {}, CommandFlagJson),
                               {},
                               apc::control::DeadlineAfter(1000));
    Check(privateAliases.Payload.find(L"\"alias\":\"Desk\"") != std::wstring::npos &&
              privateAliases.Payload.find(L"\"name\":\"\"") != std::wstring::npos &&
              privateAliases.Payload.find(L"\"displayName\":\"<device>\"") != std::wstring::npos,
          "privacy alias-list JSON must retain the characterized unredacted alias quirk while redacting identity");
    harness.Result = {};
    auto rawList = harness.Adapter.Handle(
        MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagRaw), {}, apc::control::DeadlineAfter(1000));
    Check(rawList.Payload.find(L"Desk") != std::wstring::npos &&
              rawList.Payload.find(L"device-a") != std::wstring::npos,
          "the raw flag must retain unredacted list text despite privacy mode");
}

void TestLongSnapshotIdsRemainWireVisibleAndRedactable() {
    Harness harness;
    const std::wstring longId(513, L'x');
    const auto externalId = apc::app::ExternalDeviceId::TryCreate(longId);
    Check(externalId.has_value(), "the long-ID adapter fixture must satisfy the P01 snapshot bound");
    if (!externalId) return;

    harness.Snapshot.Devices.push_back(
        {*externalId, L"Long device", {}, L"Long device", DeviceConnectionState::Connected, true, true, false});
    harness.Snapshot.Tray.ConnectedDevices.push_back(harness.Snapshot.Devices.back());
    const auto visible = harness.Adapter.Handle(
        MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagJson), {}, apc::control::DeadlineAfter(1000));
    Check(visible.Payload.find(longId) != std::wstring::npos,
          "list JSON must preserve a connected external ID beyond the persistence bound");

    harness.Snapshot.PrivacyModeEnabled = true;
    const auto privateStatus = harness.Adapter.Handle(
        MakeRequest(CommandType::Status, TargetKind::None, {}, CommandFlagJson), {}, apc::control::DeadlineAfter(1000));
    Check(privateStatus.Payload.find(L"\"id\":\"<value>\"") != std::wstring::npos &&
              privateStatus.Payload.find(longId) == std::wstring::npos,
          "status JSON must redact the same long external ID in privacy mode");
}

} // namespace

int RunControlCommandAdapterTests() {
    TestAllCommandMappingsAndTargets();
    TestInvalidRequestsAndCompatibilityGrammar();
    TestNonDeviceCommandsDoNotRequireInventorySnapshot();
    TestInventoryCommandsFailClosedOnSnapshotReadFailure();
    TestUiAndCliTypedParityAndContextPropagation();
    TestMutationBusyAndNonmutationConcurrency();
    TestResultExitMappingAndGoldenTextJsonPrivacy();
    TestLongSnapshotIdsRemainWireVisibleAndRedactable();
    return g_failures;
}
