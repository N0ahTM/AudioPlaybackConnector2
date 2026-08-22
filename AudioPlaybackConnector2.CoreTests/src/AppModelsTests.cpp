#include <app/AppModels.hpp>

#include <chrono>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace {

using apc::app::AppCommand;
using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppEvent;
using apc::app::AppResultCode;
using apc::app::DeviceConnectionState;
using apc::app::DeviceSelector;
using apc::core::DeviceId;
using apc::core::DeviceIdHash;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestDeviceIdValidationAndValueSemantics() {
    auto id = DeviceId::TryCreate(L"Bluetooth#Exact Case ");
    Check(id.has_value(), "a bounded valid device ID must be accepted");
    if (!id) return;

    Check(id->View() == L"Bluetooth#Exact Case ", "device ID validation must preserve exact platform text");
    Check(id->ToString() == L"Bluetooth#Exact Case ", "device ID string conversion must preserve exact text");
    Check(*id == *DeviceId::TryCreate(L"Bluetooth#Exact Case "), "equal IDs must compare by value");
    Check(*id != *DeviceId::TryCreate(L"Bluetooth#Other"), "different IDs must compare as different values");

    std::unordered_set<DeviceId, DeviceIdHash> ids;
    ids.insert(*id);
    Check(ids.contains(*DeviceId::TryCreate(L"Bluetooth#Exact Case ")), "equal IDs must hash to the same key");

    std::wstring oversized(513, L'x');
    std::wstring invalidUtf16(1, static_cast<wchar_t>(0xD800));
    Check(!DeviceId::TryCreate(L""), "an empty device ID must be rejected");
    Check(!DeviceId::TryCreate(std::wstring_view{L"contains\0nul", 12}), "a device ID containing NUL must be rejected");
    Check(!DeviceId::TryCreate(invalidUtf16), "an unpaired surrogate must be rejected");
    Check(!DeviceId::TryCreate(oversized), "an oversized device ID must be rejected");
}

void TestSelectorNormalizationAndContracts() {
    auto exact = DeviceSelector::ById(L"device-a");
    Check(exact.has_value() && exact->Kind() == apc::app::DeviceSelectorKind::Id,
          "an exact ID selector must retain its selector kind");
    Check(exact && exact->Id() && exact->Id()->View() == L"device-a",
          "an exact ID selector must expose a strong ID copy");
    Check(exact && exact->IdText() == L"device-a", "an exact ID selector must preserve external ID text");
    Check(exact && exact->Query().empty(), "an exact ID selector must not expose a text query");

    auto query = DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Auto, L"headphones");
    Check(query && query->Kind() == apc::app::DeviceSelectorKind::Auto && query->Query() == L"headphones",
          "a matching selector must preserve its query and kind");
    Check(!DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Last, L"ignored"),
          "stateful selectors must not accept a query payload");
    Check(!DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Name, L""),
          "a matching selector must reject an empty query");
    Check(!DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Name, std::wstring(1, L'\0')),
          "a matching selector must reject embedded NUL");
    auto longAliasQuery = DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Alias, std::wstring(129, L'x'));
    Check(longAliasQuery && longAliasQuery->Query().size() == 129,
          "an alias selector query must retain valid text beyond the persisted alias bound");
    Check(!DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Name,
                                   std::wstring(apc::app::c_maxAppCommandTextCharacters + 1, L'x')),
          "a selector query must respect the P01 payload bound");
    auto invalidUtf16Query =
        DeviceSelector::ByQuery(apc::app::DeviceSelectorKind::Name, std::wstring(1, static_cast<wchar_t>(0xD800)));
    Check(invalidUtf16Query.has_value(),
          "selector grammar must retain bounded invalid UTF-16 accepted by the existing wire validator");

    auto longExternalId = DeviceSelector::ById(std::wstring(513, L'x'));
    Check(longExternalId && longExternalId->IdText().size() == 513,
          "an external ID selector must retain valid P01 text beyond the internal DeviceId bound");
    Check(longExternalId && !longExternalId->Id(),
          "an external ID beyond the settings bound must not masquerade as a validated DeviceId");

    auto last = DeviceSelector::Last();
    auto defaultDevice = DeviceSelector::Default();
    Check(last.Kind() == apc::app::DeviceSelectorKind::Last && !last.Id() && last.Query().empty(),
          "the last selector must have no payload");
    Check(defaultDevice.Kind() == apc::app::DeviceSelectorKind::Default && !defaultDevice.Id() &&
              defaultDevice.Query().empty(),
          "the default selector must have no payload");
}

void TestCommandContractsAndNormalizedResults() {
    auto exact = DeviceSelector::ById(L"device-a");
    Check(exact.has_value(), "command contract fixture must have a valid target");
    if (!exact) return;

    AppCommand connect{AppCommandKind::Connect, exact, {}};
    Check(connect.IsWellFormed(), "connect with an exact target must be well formed");

    AppCommand alias{AppCommandKind::SetAlias, exact, L"Living room"};
    Check(alias.IsWellFormed(), "alias set with an explicit target and one-line alias must be well formed");
    AppCommand longAlias{AppCommandKind::SetAlias, exact, std::wstring(129, L'x')};
    Check(longAlias.IsWellFormed(),
          "alias set must retain valid P01 text beyond the persisted alias bound for downstream validation");
    AppCommand oversizedAlias{
        AppCommandKind::SetAlias, exact, std::wstring(apc::app::c_maxAppCommandTextCharacters + 1, L'x')};
    Check(!oversizedAlias.IsWellFormed(), "alias set must reject text beyond the P01 payload bound");
    AppCommand invalidAliasUtf16{AppCommandKind::SetAlias, exact, std::wstring(1, static_cast<wchar_t>(0xD800))};
    Check(invalidAliasUtf16.IsWellFormed(),
          "command grammar must retain bounded invalid UTF-16 accepted by the existing wire validator");

    AppCommand invalidAlias{AppCommandKind::SetAlias, exact, L"line\nwrapped"};
    Check(!invalidAlias.IsWellFormed(), "alias set must reject line breaks before persistence or transport");
    AppCommand invalidAliasNul{AppCommandKind::SetAlias, exact, std::wstring{L"abc\0nul", 7}};
    Check(!invalidAliasNul.IsWellFormed(), "alias set must reject embedded NUL");

    AppCommand invalidDefault{AppCommandKind::SetDefault, DeviceSelector::Last(), {}};
    Check(!invalidDefault.IsWellFormed(), "default set must reject implicit last-device selection");

    AppCommand invalidStatus{AppCommandKind::Status, exact, {}};
    Check(!invalidStatus.IsWellFormed(), "query commands must reject an unexpected target");

    AppCommand toggle{AppCommandKind::ToggleLast, DeviceSelector::Default(), {}};
    Check(toggle.IsWellFormed(), "toggle-last must retain its default-device selector");
    AppCommand toggleLast{AppCommandKind::ToggleLast, DeviceSelector::Last(), {}};
    Check(toggleLast.IsWellFormed(), "toggle-last must retain its last-device selector");
    AppCommand missingToggleTarget{AppCommandKind::ToggleLast, {}, {}};
    Check(!missingToggleTarget.IsWellFormed(), "toggle-last must reject a missing selector");

    apc::app::AppResult success{AppResultCode::Success, AppCommandKind::Connect};
    success.Device = apc::app::DeviceSnapshot{
        *exact->Id(), L"Headphones", {}, L"Headphones", DeviceConnectionState::Connected, true, true, false};
    apc::app::AppResult timeout{AppResultCode::TimedOut, AppCommandKind::Connect};
    Check(success.Succeeded() && !timeout.Succeeded(), "normalized result status must distinguish success and timeout");
    Check(success ==
              apc::app::AppResult{
                  AppResultCode::Success, AppCommandKind::Connect, success.Device, {}, std::nullopt, std::nullopt},
          "normalized results must have value semantics");
}

void TestSnapshotsEventsAndCommandContextAreValueOnly() {
    auto first = DeviceId::TryCreate(L"device-a");
    auto second = DeviceId::TryCreate(L"device-b");
    Check(first && second, "snapshot fixture IDs must be valid");
    if (!first || !second) return;

    apc::app::AppSnapshot snapshot;
    snapshot.Generation = 7;
    snapshot.PrivacyModeEnabled = true;
    snapshot.Devices.push_back(
        {*first, L"Headphones", {}, L"Headphones", DeviceConnectionState::Connected, true, true, false});
    snapshot.Devices.push_back(
        {*second, L"Raw Speaker", {}, L"Private device", DeviceConnectionState::Connecting, true, false, true});
    snapshot.Tray.Generation = snapshot.Generation;
    snapshot.Tray.ConnectedDevices.push_back(snapshot.Devices.front());
    snapshot.Tray.HasBusyOperations = true;

    auto copy = snapshot;
    Check(copy == snapshot, "app and tray snapshots must copy without shared mutable state");
    copy.Devices.front().DisplayName = L"Changed copy";
    Check(snapshot.Devices.front().DisplayName == L"Headphones",
          "mutating a snapshot copy must not mutate the published snapshot");
    snapshot.DefaultDevice =
        apc::app::DefaultDeviceSnapshot{apc::app::DefaultDeviceMode::SpecificDevice, *first, L"Headphones", true, true};
    snapshot.LastConnectedDeviceIds.push_back(*first);
    Check(snapshot.DefaultDevice->IsResolved && snapshot.DefaultDevice->IsConnected &&
              snapshot.LastConnectedDeviceIds.front() == *first,
          "app snapshots must carry structured default and last-device resolution data");

    AppEvent connected = apc::app::DeviceConnectedEvent{*first};
    AppEvent status = apc::app::DeviceStatusChangedEvent{*first, DeviceConnectionState::Connected};
    Check(std::holds_alternative<apc::app::DeviceConnectedEvent>(connected) &&
              std::holds_alternative<apc::app::DeviceStatusChangedEvent>(status),
          "device facts must remain typed variants rather than stringly events");
    const std::wstring longEventId(513, L'e');
    const auto externalEventId = apc::app::ExternalDeviceId::TryCreate(longEventId);
    AppEvent longConnected = apc::app::DeviceConnectedEvent{*externalEventId};
    Check(externalEventId && std::get<apc::app::DeviceConnectedEvent>(longConnected).Id.View() == longEventId &&
              !std::get<apc::app::DeviceConnectedEvent>(longConnected).Id.Bounded(),
          "typed device events must use the P01 external identity rather than the persistence-bounded DeviceId");
    AppEvent activity = apc::app::DeviceActivityChangedEvent{};
    AppEvent inventory = apc::app::DeviceInventoryChangedEvent{};
    Check(std::holds_alternative<apc::app::DeviceActivityChangedEvent>(activity) &&
              std::holds_alternative<apc::app::DeviceInventoryChangedEvent>(inventory),
          "empty activity and inventory facts must preserve the legacy event payload contract");

    AppCommandContext context;
    Check(!context.IsExpired(AppCommandContext::TimePoint{}), "an unlimited command context must not expire");
    context.Deadline = AppCommandContext::TimePoint{} + std::chrono::seconds(2);
    Check(context.IsExpired(AppCommandContext::TimePoint{} + std::chrono::seconds(2)),
          "a command deadline must expire at its exact boundary");
}

void TestExternalSnapshotIdRetainsProtocolLengthWithoutWeakeningDeviceId() {
    const std::wstring longId(513, L'x');
    const auto external = apc::app::ExternalDeviceId::TryCreate(longId);
    Check(external && external->View() == longId && !external->Bounded(),
          "external snapshot IDs must retain valid P01 text while exposing no invalid persistence identity");
    const auto bounded = apc::core::DeviceId::TryCreate(L"device-a");
    Check(bounded && apc::app::ExternalDeviceId{*bounded}.Bounded() == bounded,
          "external snapshot IDs must round-trip bounded DeviceId values");
}

} // namespace

int RunAppModelsTests() {
    TestDeviceIdValidationAndValueSemantics();
    TestSelectorNormalizationAndContracts();
    TestCommandContractsAndNormalizedResults();
    TestSnapshotsEventsAndCommandContextAreValueOnly();
    TestExternalSnapshotIdRetainsProtocolLengthWithoutWeakeningDeviceId();
    return g_failures;
}
