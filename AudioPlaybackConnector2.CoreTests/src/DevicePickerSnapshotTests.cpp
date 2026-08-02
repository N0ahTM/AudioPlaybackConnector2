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
    Check(!cache.IsInventoryFresh(At(6s)), "an inventory event must invalidate even a young snapshot");
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
    Check(second.Generation > first.Generation, "every coherent refresh must publish a new snapshot generation");
    Check(cache.CanSelect(L"a") && cache.CanSelect(L"b"), "idle discovered devices must remain selectable");

    cache.Clear();
    Check(!cache.HasInventory() && cache.CachedSnapshot().Items.empty(),
          "release must discard both native inventory and presentation rows");
    Check(cache.CachedSnapshot().Generation > second.Generation,
          "releasing a cache must not make snapshot generations move backwards");
}
} // namespace

int RunDevicePickerSnapshotTests() {
    TestInventoryFreshnessAndInvalidation();
    TestConcurrentInventoryGeneration();
    TestLateInventoryCallbackAfterRelease();
    TestConsistentPresentationSnapshot();
    TestPrivacyAndSnapshotGeneration();
    return g_failures;
}
