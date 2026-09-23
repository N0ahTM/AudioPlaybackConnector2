#include "TestCheck.hpp"
#include "AppTestFixture.hpp"
#include <semaphore>

#include <control/ControlCommandAdapter.hpp>
#include <control/CommandPipeIo.hpp>

#include <winrt/Windows.Data.Json.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
    apc::tests::AppFixture Fixture;
    AppController& Controller = Fixture.Controller;
    ControlCommandAdapter Adapter{Controller, {Localize}};
    Harness() {
        (void)Fixture.Settings->RecordConnectedDevice(L"device-a", L"Headphones");
        (void)Fixture.Settings->RememberDevice(L"device-b", L"Speaker");
        (void)Fixture.Settings->SetDeviceAlias(L"device-a", L"Desk");
        (void)Fixture.Settings->SetDefaultDevice(L"device-a");
        apc::tests::device::ConnectSuccessfully(*Fixture.Devices, L"device-a");
    }
};

void TestMappingsUseConcreteOwners() {
    Harness harness;
    for (auto command : {CommandType::Show,
                         CommandType::Settings,
                         CommandType::List,
                         CommandType::Status,
                         CommandType::DefaultShow,
                         CommandType::AliasList}) {
        Check(harness.Adapter.Handle(MakeRequest(command), {}, apc::control::DeadlineAfter(1000)).Code ==
                  ExitCode::Success,
              "queries and UI commands must reach the concrete application owners");
    }
    Check(harness.Fixture.Presentation->Modes == std::vector{apc::app::DevicePickerOpenMode::EnsureOpen} &&
              harness.Fixture.Presentation->SettingsCalls == 1,
          "control presentation must retain ensure-open picker semantics");
    auto response = harness.Adapter.Handle(
        MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a\nOffice"), {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success &&
              harness.Fixture.Settings->Snapshot().Data.Devices.front().Alias == L"Office",
          "AliasSet must split target and alias and persist the requested value");
    response = harness.Adapter.Handle(
        MakeRequest(CommandType::DefaultSet, TargetKind::Name, L"Speaker"), {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success &&
              harness.Fixture.Settings->Snapshot().Data.DefaultDeviceId == L"device-b",
          "named DefaultSet must resolve and persist the matching identity");
    response = harness.Adapter.Handle(
        MakeRequest(CommandType::Connect, TargetKind::Last), {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success && response.Payload == L"Already connected: Office",
          "Last must resolve the recorded connected target rather than the selected default");
    response = harness.Adapter.Handle(
        MakeRequest(CommandType::Disconnect, TargetKind::Alias, L"Office"), {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::Success && !harness.Fixture.Service->IsDeviceConnected(L"device-a"),
          "alias disconnect must mutate the resolved concrete session");
    apc::tests::device::CompleteCloseAndCooldown(*harness.Fixture.Devices,
                                                 harness.Fixture.Devices->ConnectionAccess->LastConnection);
    for (auto command :
         {CommandType::DefaultClear, CommandType::AliasClear, CommandType::DisconnectAll, CommandType::ReconnectAll}) {
        auto request = command == CommandType::AliasClear ? MakeRequest(command, TargetKind::Id, L"device-a")
                                                          : MakeRequest(command);
        Check(harness.Adapter.Handle(request, {}, apc::control::DeadlineAfter(1000)).Code == ExitCode::Success,
              "clear and bulk commands must execute through concrete owners");
    }
}

void TestInvalidRequestsAndCompatibilityGrammar() {
    Harness harness;
    auto const before = harness.Fixture.Settings->Snapshot().Revision;
    for (auto const& request : {MakeRequest(CommandType::Status, TargetKind::Id, L"device-a"),
                                MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a"),
                                MakeRequest(CommandType::ToggleLast)}) {
        Check(harness.Adapter.Handle(request, {}, apc::control::DeadlineAfter(1000)).Code == ExitCode::InvalidRequest,
              "invalid wire grammar must be rejected before application actions");
    }
    Check(harness.Fixture.Settings->Snapshot().Revision == before, "invalid requests must not mutate settings");
    std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
    auto response = harness.Adapter.Handle(
        MakeRequest(CommandType::Connect, TargetKind::Name, invalidUtf16), {}, apc::control::DeadlineAfter(1000));
    Check(response.Code == ExitCode::NotFound, "transport-compatible UTF-16 must reach target resolution");
}

void TestControllerPreDispatchTerminationIsUnavailableForEveryCommand() {
    const std::vector<Request> requests{MakeRequest(CommandType::Show),
                                        MakeRequest(CommandType::Settings),
                                        MakeRequest(CommandType::List),
                                        MakeRequest(CommandType::Status),
                                        MakeRequest(CommandType::DefaultShow),
                                        MakeRequest(CommandType::DefaultSet, TargetKind::Id, L"device-a"),
                                        MakeRequest(CommandType::DefaultClear),
                                        MakeRequest(CommandType::AliasList),
                                        MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a\nDesk"),
                                        MakeRequest(CommandType::AliasClear, TargetKind::Id, L"device-a"),
                                        MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"),
                                        MakeRequest(CommandType::Disconnect, TargetKind::Id, L"device-a"),
                                        MakeRequest(CommandType::Reconnect, TargetKind::Id, L"device-a"),
                                        MakeRequest(CommandType::ToggleLast, TargetKind::Default),
                                        MakeRequest(CommandType::DisconnectAll),
                                        MakeRequest(CommandType::ReconnectAll)};
    Harness harness;
    auto const revision = harness.Fixture.Settings->Snapshot().Revision;
    std::stop_source stop;
    stop.request_stop();
    for (auto const& request : requests) {
        Check(harness.Adapter.Handle(request, stop.get_token(), 0).Code == ExitCode::Unavailable &&
                  harness.Adapter.Handle(request, {}, GetTickCount64()).Code == ExitCode::Unavailable,
              "pre-dispatch cancellation and deadline must preserve exit 7 for every command");
    }
    Check(harness.Fixture.Settings->Snapshot().Revision == revision &&
              harness.Fixture.Service->IsDeviceConnected(L"device-a") &&
              harness.Fixture.Presentation->SettingsCalls == 0 && harness.Fixture.Presentation->Modes.empty(),
          "preflight termination must not mutate any owner or enter presentation");
}

void TestControllerPostDispatchTerminationRemainsIndeterminate() {
    for (auto status : {apc::app::AppActionStatus::Cancelled,
                        apc::app::AppActionStatus::TimedOut,
                        apc::app::AppActionStatus::Indeterminate}) {
        Harness harness;
        harness.Fixture.Presentation->SettingsAction = [status](auto const&) {
            return apc::app::AppUiActionResult{status, std::nullopt};
        };
        auto response =
            harness.Adapter.Handle(MakeRequest(CommandType::Settings), {}, apc::control::DeadlineAfter(1000));
        Check(response.Code == ExitCode::Indeterminate && harness.Fixture.Presentation->SettingsCalls == 1,
              "a termination after presentation admission must preserve exit 9");
    }
}

void TestMutationBusyAndNonmutationConcurrency() {
    Harness harness;
    std::binary_semaphore entered(0), release(0);
    harness.Fixture.Devices->WatcherAccess->BeforeRefreshCompletion = [&] {
        entered.release();
        release.acquire();
    };
    std::jthread first([&] {
        (void)harness.Adapter.Handle(
            MakeRequest(CommandType::DefaultSet, TargetKind::Name, L"Speaker"), {}, apc::control::DeadlineAfter(5000));
    });
    entered.acquire();
    Check(harness.Adapter.Handle(MakeRequest(CommandType::DefaultClear), {}, apc::control::DeadlineAfter(1000)).Code ==
              ExitCode::Busy,
          "a concurrent mutation must receive Busy while another action is admitted");
    Check(harness.Adapter.Handle(MakeRequest(CommandType::Status), {}, apc::control::DeadlineAfter(1000)).Code ==
              ExitCode::Success,
          "a read-only query must proceed while target resolution is blocked");
    release.release();
    first.join();
}

void TestAdapterUsesTheProductionFormatter() {
    Harness harness;
    (void)harness.Fixture.Settings->SetPrivacyModeEnabled(true);
    for (auto flags :
         {std::uint32_t{0}, std::uint32_t{CommandFlagJson}, std::uint32_t{CommandFlagJson | CommandFlagRaw}}) {
        auto input = MakeRequest(CommandType::Status, TargetKind::None, {}, flags);
        auto result = harness.Controller.Status({});
        auto expected = ControlCommandAdapter::FormatResponse(input, result, {Localize});
        auto actual = harness.Adapter.Handle(input, {}, apc::control::DeadlineAfter(1000));
        Check(expected.Code == actual.Code && expected.Payload == actual.Payload &&
                  actual.CorrelationId == input.CorrelationId,
              "the live adapter must use the production formatter for typed results and privacy modes");
    }
}

void TestQueryAdapterUsesOnlyItsResultSnapshot() {
    Harness harness;
    int captures = 0;
    harness.Fixture.Presentation->BeforeResourceRead = [&] { ++captures; };
    auto result = harness.Adapter.Handle(MakeRequest(CommandType::Status), {}, apc::control::DeadlineAfter(1000));
    Check(result.Code == ExitCode::Success && captures == 1,
          "a control query must capture once through its use case instead of reading a separate fallback snapshot");
    bool privacy = false;
    harness.Fixture.Presentation->BeforeResourceRead = [&] {
        privacy = !privacy;
        (void)harness.Fixture.Settings->SetPrivacyModeEnabled(privacy);
    };
    result = harness.Adapter.Handle(MakeRequest(CommandType::Status), {}, apc::control::DeadlineAfter(1000));
    Check(result.Code == ExitCode::Unavailable,
          "an unstable application query must not be rescued by an older transport snapshot");
}

struct FormattingFixture {
    AppResult Result;
    AppSnapshot Snapshot = FixtureSnapshot();

    Response FormatStarted(Request const& request) const {
        auto result = Result;
        result.DispatchPhase = apc::app::AppDispatchPhase::Started;
        if (!result.Snapshot &&
            (request.Command == CommandType::List || request.Command == CommandType::Status ||
             request.Command == CommandType::DefaultShow || request.Command == CommandType::AliasList))
            result.Snapshot = Snapshot;
        return ControlCommandAdapter::FormatResponse(request, result, {Localize});
    }
};

void TestResultExitMappingAndGoldenTextJsonPrivacy() {
    FormattingFixture harness;

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
        harness.Result.Command = AppCommandKind::Connect;
        harness.Result.Reason =
            code.AppCode == AppResultCode::OperationFailed ? AppOutcomeReason::ConnectFailed : AppOutcomeReason::None;
        if (code.AppCode == AppResultCode::InternalError) {
            auto result = harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"));
            Check(result.Code == code.WireCode && result.Payload.empty(),
                  "internal controller errors must preserve the legacy empty indeterminate payload");
            continue;
        }
        auto result = harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"));
        Check(result.Code == code.WireCode, "every normalized AppResultCode must map to its P01 exit code");
    }

    harness.Result = {};
    harness.Result.Code = AppResultCode::Success;
    harness.Result.Command = AppCommandKind::ListDevices;
    auto listText = harness.FormatStarted(MakeRequest(CommandType::List));
    Check(listText.Payload == L"Devices\n- Desk (connected)\n  ID: device-a\n- Speaker\n  ID: device-b\n",
          "list text must preserve legacy ordering, connected suffix, and ID shape");

    auto listJson = harness.FormatStarted(MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagJson));
    Check(listJson.Payload == L"{\"devices\":[{\"id\":\"device-a\",\"name\":\"Headphones\",\"alias\":\"Desk\","
                              L"\"displayName\":\"Desk\",\"connected\":true,\"known\":true,\"privacyRedacted\":false},{"
                              L"\"id\":\"device-b\",\"name\":\"Speaker\",\"alias\":\"\",\"displayName\":\"Speaker\","
                              L"\"connected\":false,\"known\":true,\"privacyRedacted\":false}]}",
          "list JSON must preserve the established device object shape and field order");

    harness.Result.Command = AppCommandKind::Status;
    auto statusText = harness.FormatStarted(MakeRequest(CommandType::Status));
    Check(statusText.Payload == L"Running\nConnections: 1\n- Desk\n  ID: device-a\n",
          "status text must preserve the running/count/device presentation shape");

    auto statusJson = harness.FormatStarted(MakeRequest(CommandType::Status, TargetKind::None, {}, CommandFlagJson));
    Check(statusJson.Payload ==
              L"{\"running\":true,\"connectedCount\":1,\"connectedDevices\":[{\"id\":\"device-a\",\"name\":"
              L"\"Headphones\",\"alias\":\"Desk\",\"displayName\":\"Desk\",\"connected\":true,\"known\":true,"
              L"\"privacyRedacted\":false}],\"devicePickerOpenedGeneration\":12,\"adaptiveResources\":{\"evaluated\":"
              L"true,\"residency\":\"Hot\",\"backgroundResidency\":\"Warm\",\"snapshotFresh\":true,"
              L"\"positiveAuthorizationCurrent\":true,\"preloadAllowed\":true,\"uiResourcesLoaded\":true,"
              L"\"uiResourcesInitialized\":true,\"memoryPressure\":\"Low\",\"userActivity\":\"Available\","
              L"\"energySaver\":false}}",
          "status JSON must retain picker generation and complete adaptive-resource diagnostics");

    harness.Result.Command = AppCommandKind::ShowDefault;
    auto defaultJson =
        harness.FormatStarted(MakeRequest(CommandType::DefaultShow, TargetKind::None, {}, CommandFlagJson));
    Check(defaultJson.Payload == L"{\"ok\":true,\"mode\":\"specificDevice\",\"privacyRedacted\":false,\"id\":\"device-"
                                 L"a\",\"displayName\":\"Desk\",\"resolved\":true,\"connected\":true}",
          "default JSON must retain mode, resolved, connected, and privacy fields");
    auto defaultText = harness.FormatStarted(MakeRequest(CommandType::DefaultShow));
    Check(defaultText.Payload == L"Default: Desk\n", "default text must preserve the localized specific-device shape");

    harness.Result.Command = AppCommandKind::ListAliases;
    auto aliasJson = harness.FormatStarted(MakeRequest(CommandType::AliasList, TargetKind::None, {}, CommandFlagJson));
    Check(aliasJson.Payload ==
              L"{\"devices\":[{\"id\":\"device-a\",\"name\":\"Headphones\",\"alias\":\"Desk\",\"displayName\":\"Desk\","
              L"\"connected\":true,\"known\":true,\"privacyRedacted\":false,\"hasAlias\":true},{\"id\":\"device-b\","
              L"\"name\":\"Speaker\",\"alias\":\"\",\"displayName\":\"Speaker\",\"connected\":false,\"known\":true,"
              L"\"privacyRedacted\":false,\"hasAlias\":false}],\"privacyRedacted\":false}",
          "alias-list JSON must retain per-device hasAlias and root privacy fields");
    auto aliasText = harness.FormatStarted(MakeRequest(CommandType::AliasList));
    Check(aliasText.Payload == L"Aliases\n- Desk: Desk\n  ID: device-a\n- Speaker: (none)\n  ID: device-b\n",
          "alias-list text must preserve alias labels, empty-alias text, and IDs");

    harness.Result = {};
    harness.Result.Code = AppResultCode::Success;
    harness.Result.Reason = AppOutcomeReason::ConnectSucceeded;
    harness.Result.Target = apc::app::AppTargetSnapshot{L"device-a", L"Headphones", L"Desk", L"Desk", true, true, true};
    harness.Result.Command = AppCommandKind::Connect;
    auto connectText = harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a"));
    Check(connectText.Payload == L"Connected: Desk",
          "connect operation text must preserve its localized action message");
    auto connectJson =
        harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a", CommandFlagJson));
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
    harness.Result.Command = AppCommandKind::SetAlias;
    auto aliasOperation = harness.FormatStarted(
        MakeRequest(CommandType::AliasSet, TargetKind::Id, L"device-a\nNew Alias", CommandFlagJson));
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
    harness.Result.Command = AppCommandKind::ReconnectAll;
    auto reconnectAllFailure =
        harness.FormatStarted(MakeRequest(CommandType::ReconnectAll, TargetKind::None, {}, CommandFlagJson));
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
    harness.Result.Command = AppCommandKind::Connect;
    auto longIdFailure =
        harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, longId, CommandFlagJson));
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
    harness.Result.Command = AppCommandKind::Connect;
    auto privateFailure =
        harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Id, L"device-a", CommandFlagJson));
    Check(privateFailure.Payload.find(L"\"id\":\"<value>\"") != std::wstring::npos &&
              privateFailure.Payload.find(L"\"name\":\"<device>\"") != std::wstring::npos &&
              privateFailure.Payload.find(L"Connect failed: <device>") != std::wstring::npos,
          "known-target operation failures must redact ID, name, and message in privacy mode");

    harness.Result = {};
    harness.Result.Code = AppResultCode::NotFound;
    harness.Result.Reason = AppOutcomeReason::TargetNotFound;
    harness.Result.RequestedTarget = L"Headphones";
    harness.Result.PrivacyModeEnabled = true;
    harness.Result.Command = AppCommandKind::Connect;
    auto privateQueryFailure =
        harness.FormatStarted(MakeRequest(CommandType::Connect, TargetKind::Name, L"Headphones", CommandFlagJson));
    Check(privateQueryFailure.Payload.find(L"Target not found: Headphones") != std::wstring::npos,
          "not-found messages must preserve the current raw query compatibility quirk");

    harness.Snapshot.PrivacyModeEnabled = true;
    harness.Result = {};
    harness.Result.Command = AppCommandKind::ListAliases;
    auto privateAliases =
        harness.FormatStarted(MakeRequest(CommandType::AliasList, TargetKind::None, {}, CommandFlagJson));
    Check(privateAliases.Payload.find(L"\"alias\":\"Desk\"") != std::wstring::npos &&
              privateAliases.Payload.find(L"\"name\":\"\"") != std::wstring::npos &&
              privateAliases.Payload.find(L"\"displayName\":\"<device>\"") != std::wstring::npos,
          "privacy alias-list JSON must retain the characterized unredacted alias quirk while redacting identity");
    harness.Result = {};
    harness.Result.Command = AppCommandKind::ListDevices;
    auto rawList = harness.FormatStarted(MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagRaw));
    Check(rawList.Payload.find(L"Desk") != std::wstring::npos &&
              rawList.Payload.find(L"device-a") != std::wstring::npos,
          "the raw flag must retain unredacted list text despite privacy mode");
}

void TestLongSnapshotIdsRemainWireVisibleAndRedactable() {
    FormattingFixture harness;
    const std::wstring longId(513, L'x');
    const auto externalId = apc::app::ExternalDeviceId::TryCreate(longId);
    Check(externalId.has_value(), "the long-ID adapter fixture must satisfy the P01 snapshot bound");
    if (!externalId) return;

    harness.Snapshot.Devices.push_back(
        {*externalId, L"Long device", {}, L"Long device", DeviceConnectionState::Connected, true, true, false});
    harness.Snapshot.Tray.ConnectedDevices.push_back(harness.Snapshot.Devices.back());
    harness.Result.Command = AppCommandKind::ListDevices;
    const auto visible = harness.FormatStarted(MakeRequest(CommandType::List, TargetKind::None, {}, CommandFlagJson));
    Check(visible.Payload.find(longId) != std::wstring::npos,
          "list JSON must preserve a connected external ID beyond the persistence bound");

    harness.Snapshot.PrivacyModeEnabled = true;
    harness.Result.Command = AppCommandKind::Status;
    const auto privateStatus =
        harness.FormatStarted(MakeRequest(CommandType::Status, TargetKind::None, {}, CommandFlagJson));
    Check(privateStatus.Payload.find(L"\"id\":\"<value>\"") != std::wstring::npos &&
              privateStatus.Payload.find(longId) == std::wstring::npos,
          "status JSON must redact the same long external ID in privacy mode");
}

} // namespace

int RunControlCommandAdapterTests() {
    TestMappingsUseConcreteOwners();
    TestInvalidRequestsAndCompatibilityGrammar();
    TestControllerPreDispatchTerminationIsUnavailableForEveryCommand();
    TestControllerPostDispatchTerminationRemainsIndeterminate();
    TestMutationBusyAndNonmutationConcurrency();
    TestAdapterUsesTheProductionFormatter();
    TestQueryAdapterUsesOnlyItsResultSnapshot();
    TestResultExitMappingAndGoldenTextJsonPrivacy();
    TestLongSnapshotIdsRemainWireVisibleAndRedactable();
    return g_failures;
}
