#include <core/DevicePickerSnapshot.hpp>

#include <chrono>
#include <iostream>
#include <string_view>

namespace {
using namespace std::chrono_literals;
using apc::device_picker::DeviceActivitySnapshot;
using apc::device_picker::DeviceIdentity;
using apc::device_picker::DeviceInventoryGeneration;
using apc::device_picker::DevicePickerSnapshotCache;
using apc::device_picker::DevicePresentationSetting;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

DevicePickerSnapshotCache::TimePoint At(std::chrono::seconds elapsed) {
    return DevicePickerSnapshotCache::TimePoint{} + elapsed;
}

void TestInventoryFreshnessAndInvalidation() {
    DevicePickerSnapshotCache cache(10s);
    Check(!cache.HasInventory(), "an uninitialized cache must not masquerade as an empty device result");
    Check(!cache.IsInventoryFresh(At(0s)), "an uninitialized inventory must be stale");

    cache.ReplaceInventory({}, At(5s));
    Check(cache.HasInventory(), "a successful empty enumeration must be cached");
    Check(cache.IsInventoryFresh(At(5s)), "a new inventory must be fresh immediately");
    Check(cache.IsInventoryFresh(At(15s)), "freshness must include the exact lifetime boundary");
    Check(!cache.IsInventoryFresh(At(15s) + 1ms), "inventory must expire after the lifetime boundary");
    Check(!cache.IsInventoryFresh(At(4s)), "clock rollback must not make an inventory fresh");

    DeviceActivitySnapshot activity;
    auto const firstInventoryGeneration = cache.Refresh(activity, {}, false, L"Private", At(6s)).InventoryGeneration;
    cache.InvalidateInventory();
    auto const invalidatedInventoryGeneration = cache.CachedSnapshot().InventoryGeneration;
    cache.InvalidateInventory();
    Check(!cache.IsInventoryFresh(At(6s)), "an inventory event must invalidate even a young snapshot");
    Check(cache.CachedSnapshot().InventoryGeneration == invalidatedInventoryGeneration,
          "repeated invalidation of an already stale inventory must be idempotent");
    auto const& invalidated = cache.Refresh(activity, {}, false, L"Private", At(6s));
    Check(!invalidated.InventoryFresh, "invalid freshness must be visible in the published snapshot");
    Check(invalidated.InventoryGeneration != firstInventoryGeneration,
          "inventory invalidation must advance its generation");
}

void TestConcurrentInventoryGeneration() {
    DeviceInventoryGeneration generation;
    auto const started = generation.Capture();
    Check(!generation.ChangedSince(started), "a refresh generation must remain stable without inventory events");
    generation.Invalidate();
    generation.Invalidate();
    Check(generation.ChangedSince(started), "events during enumeration must require a follow-up refresh");
    auto const followUp = generation.Capture();
    Check(!generation.ChangedSince(followUp), "multiple prior events must coalesce into one follow-up generation");
}

void TestLateInventoryCallbackAfterRelease() {
    DeviceInventoryGeneration generation;
    auto const beforeRelease = generation.Capture();
    generation.Deactivate();
    Check(!generation.TryInvalidate(), "an inventory callback after release must be rejected");
    Check(!generation.ChangedSince(beforeRelease), "a rejected late callback must not mutate released view state");
}

void TestConsistentPresentationSnapshot() {
    DevicePickerSnapshotCache cache;
    cache.ReplaceInventory(
        {
            DeviceIdentity{L"a", L"Enumerated A"},
            DeviceIdentity{L"b", L""},
            DeviceIdentity{L"a", L"Duplicate A"},
            DeviceIdentity{L"", L"Invalid"},
        },
        At(0s));

    DeviceActivitySnapshot activity;
    activity.ConnectedIds = {L"a", L"connected-but-not-discovered"};
    activity.BusyIds = {L"b"};
    std::vector settings{
        DevicePresentationSetting{L"a", L"Remembered A", L"Living room"},
        DevicePresentationSetting{L"a", L"Ignored duplicate", L"Ignored alias"},
    };

    auto const& snapshot = cache.Refresh(activity, settings, false, L"Private device", At(1s));
    Check(snapshot.Items.size() == 2, "empty and duplicate device IDs must not create picker rows");
    Check(snapshot.ConnectedDeviceCount == 2,
          "the global connected count must include sessions outside the discovered inventory");
    Check(snapshot.Items[0].Id == L"a" && snapshot.Items[0].Name == L"Remembered A",
          "remembered names must override transient enumeration names");
    Check(snapshot.Items[0].Alias == L"Living room" && snapshot.Items[0].DisplayName == L"Living room",
          "aliases must be retained and used as the display name");
    Check(snapshot.Items[0].IsConnected && !snapshot.Items[0].IsBusy,
          "connected and busy state must come from the same activity snapshot");
    Check(snapshot.Items[1].Name == L"b" && snapshot.Items[1].DisplayName == L"b",
          "an empty enumerated name must fall back to the stable device ID");
    Check(!snapshot.Items[1].IsConnected && snapshot.Items[1].IsBusy, "busy state must be mapped by device ID");
    Check(!cache.CanSelect(L"a") && !cache.CanSelect(L"b") && !cache.CanSelect(L"missing"),
          "connected, busy, and unknown rows must not be selectable");
}

void TestSavedDevicesRemainConfigurable() {
    DevicePickerSnapshotCache cache;
    cache.ReplaceInventory({DeviceIdentity{L"a", L"Available"}}, At(0s));
    DeviceActivitySnapshot activity;
    std::vector settings{
        DevicePresentationSetting{L"a", L"Available", L""},
        DevicePresentationSetting{L"b", L"Saved", L"Desk", true},
        DevicePresentationSetting{L"b", L"Duplicate", L"Ignored"},
        DevicePresentationSetting{L"", L"Invalid", L""},
    };

    auto first = cache.Refresh(activity, settings, false, L"Private", At(1s));
    Check(first.Items.size() == 2, "saved and discovered devices must share one deduplicated list");
    Check(first.Items[0].IsAvailable && cache.CanSelect(L"a"), "discovered idle devices must remain connectable");
    Check(!first.Items[1].IsAvailable && !cache.CanSelect(L"b"),
          "an unavailable saved device must remain listed without offering a connection");
    Check(first.Items[1].DisplayName == L"Desk" && first.Items[1].IsDefault,
          "saved devices must retain their alias and default marker");
    Check(cache.Refresh(activity, settings, false, L"Private", At(2s)).Generation == first.Generation,
          "an unchanged saved-device list must not trigger another render");

    cache.ReplaceInventory({}, At(3s));
    auto missing = cache.Refresh(activity, settings, true, L"Private", At(3s));
    Check(missing.Items.size() == 2 && !missing.Items[0].IsAvailable && !missing.Items[1].IsAvailable,
          "an empty discovery result must not hide saved settings");
    Check(missing.Items[0].DisplayName == L"Private" && missing.Items[1].DisplayName == L"Desk",
          "saved unavailable devices must obey the same privacy rules as discovered devices");

    activity.ConnectedIds.insert(L"b");
    auto connected = cache.Refresh(activity, settings, false, L"Private", At(4s));
    Check(connected.Items[1].IsAvailable && connected.Items[1].IsConnected && !cache.CanSelect(L"b"),
          "a live saved connection must remain visible even while discovery is empty");
    activity.ConnectedIds.clear();
    activity.BusyIds.insert(L"b");
    auto busy = cache.Refresh(activity, settings, false, L"Private", At(5s));
    Check(busy.Items[1].IsAvailable && busy.Items[1].IsBusy && !cache.CanSelect(L"b"),
          "an in-progress saved connection must stay visible without allowing a duplicate action");
    activity.BusyIds.clear();
    cache.ReplaceInventory({DeviceIdentity{L"b", L"Rediscovered"}}, At(6s));
    auto rediscovered = cache.Refresh(activity, settings, false, L"Private", At(6s));
    Check(rediscovered.Items.size() == 2 && rediscovered.Items[0].Id == L"b" && cache.CanSelect(L"b"),
          "rediscovery must promote the existing saved row to an available row without duplicating it");
    settings[0].IsDefault = true;
    settings[1].IsDefault = false;
    auto changedDefault = cache.Refresh(activity, settings, false, L"Private", At(7s));
    Check(changedDefault.Generation > rediscovered.Generation && !changedDefault.Items[0].IsDefault &&
              changedDefault.Items[1].IsDefault,
          "moving the default marker must update both rows and invalidate the presentation");
}

void TestPrivacyAndSnapshotGeneration() {
    DevicePickerSnapshotCache cache;
    cache.ReplaceInventory({DeviceIdentity{L"a", L"Headphones"}, DeviceIdentity{L"b", L"Speaker"}}, At(0s));

    DeviceActivitySnapshot idle;
    std::vector settings{DevicePresentationSetting{L"a", L"", L"Trusted alias"}};
    auto first = cache.Refresh(idle, settings, true, L"Private device", At(1s));
    Check(first.PrivacyModeEnabled, "privacy state must be part of the immutable picker snapshot");
    Check(first.Items[0].DisplayName == L"Trusted alias", "an explicit alias must remain visible in privacy mode");
    Check(first.Items[1].DisplayName == L"Private device", "privacy mode must redact unaliased device names");
    Check(first.Items[1].Name == L"Speaker", "redaction must not destroy the cached native name");

    auto second = cache.Refresh(idle, settings, true, L"Private device", At(2s));
    Check(second.Generation == first.Generation, "an identical refresh must retain the published snapshot generation");
    Check(cache.CanSelect(L"a") && cache.CanSelect(L"b"), "idle discovered devices must remain selectable");

    cache.InvalidateInventory();
    auto metadataOnly = cache.Refresh(idle, settings, true, L"Private device", At(2s));
    Check(metadataOnly.Generation == second.Generation,
          "inventory metadata changes without presentation changes must not invalidate the XAML render key");

    idle.BusyIds.insert(L"a");
    auto changed = cache.Refresh(idle, settings, true, L"Private device", At(3s));
    Check(changed.Generation > second.Generation, "a presentation change must publish a new snapshot generation");
    auto unchanged = cache.Refresh(idle, settings, true, L"Private device", At(4s));
    Check(unchanged.Generation == changed.Generation,
          "a repeated presentation snapshot must not advance its generation");

    cache.Clear();
    Check(!cache.HasInventory() && cache.CachedSnapshot().Items.empty(),
          "release must discard both native inventory and presentation rows");
    Check(cache.CachedSnapshot().Generation > unchanged.Generation,
          "releasing a cache must not make snapshot generations move backwards");
}
} // namespace

int RunDevicePickerSnapshotTests() {
    TestInventoryFreshnessAndInvalidation();
    TestConcurrentInventoryGeneration();
    TestLateInventoryCallbackAfterRelease();
    TestConsistentPresentationSnapshot();
    TestSavedDevicesRemainConfigurable();
    TestPrivacyAndSnapshotGeneration();
    return g_failures;
}
