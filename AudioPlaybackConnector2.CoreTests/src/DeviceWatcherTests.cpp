#include <core/DeviceWatcher.hpp>

#include <algorithm>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apc::device::DeviceWatcher;
using apc::device::DeviceWatcherCallbacks;
using apc::device::DeviceWatcherFact;
using apc::device::DeviceWatcherFactKind;
using apc::device::DeviceWatcherPlatform;
using apc::device::DeviceWatcherRegistration;
using apc::device_picker::DeviceIdentity;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class ManualExecutor {
public:
    void Post(DeviceWatcher::Task task) { m_tasks.push_back(std::move(task)); }

    void RunOne() {
        auto task = std::move(m_tasks.front());
        m_tasks.pop_front();
        task();
    }

    void RunAll() {
        while (!m_tasks.empty())
            RunOne();
    }

    [[nodiscard]] std::size_t PendingCount() const noexcept { return m_tasks.size(); }

private:
    std::deque<DeviceWatcher::Task> m_tasks;
};

class FakeDeviceInformationWatcherState {
public:
    explicit FakeDeviceInformationWatcherState(DeviceWatcherCallbacks callbacks) : Callbacks(std::move(callbacks)) {}

    void Add(std::wstring id, std::wstring name) { Callbacks.DeviceAdded({std::move(id), std::move(name)}); }
    void Remove(std::wstring id) { Callbacks.DeviceRemoved(std::move(id)); }
    void CompleteEnumeration() { Callbacks.EnumerationCompleted(); }

    int StartCalls = 0;
    int StopCalls = 0;
    int RevokeCalls = 0;
    bool FailStart = false;

private:
    DeviceWatcherCallbacks Callbacks;
};

class FakeDeviceInformationWatcher final : public DeviceWatcherRegistration {
public:
    explicit FakeDeviceInformationWatcher(std::shared_ptr<FakeDeviceInformationWatcherState> state)
        : m_state(std::move(state)) {}

    void Start() override {
        ++m_state->StartCalls;
        if (m_state->FailStart) throw std::runtime_error("planned watcher start failure");
    }
    void Stop() noexcept override { ++m_state->StopCalls; }
    void RevokeCallbacks() noexcept override { ++m_state->RevokeCalls; }

private:
    std::shared_ptr<FakeDeviceInformationWatcherState> m_state;
};

class FakeDeviceWatcherPlatform final : public DeviceWatcherPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceWatcherRegistration>
    CreateDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) override {
        auto state = std::make_shared<FakeDeviceInformationWatcherState>(std::move(callbacks));
        state->FailStart = FailNextStart;
        FailNextStart = false;
        LastWatcher = state;
        Watchers.push_back(std::move(state));
        return std::make_unique<FakeDeviceInformationWatcher>(LastWatcher);
    }

    std::shared_ptr<FakeDeviceInformationWatcherState> LastWatcher;
    std::vector<std::shared_ptr<FakeDeviceInformationWatcherState>> Watchers;
    bool FailNextStart = false;
};

struct Fixture {
    Fixture()
        : PlatformAccess(Platform.get()), Watcher([this](DeviceWatcher::Task task) { Executor.Post(std::move(task)); },
                                                  [this](DeviceWatcherFact const& fact) { Facts.push_back(fact); },
                                                  std::move(Platform)) {}

    ManualExecutor Executor;
    std::unique_ptr<FakeDeviceWatcherPlatform> Platform = std::make_unique<FakeDeviceWatcherPlatform>();
    FakeDeviceWatcherPlatform* PlatformAccess = nullptr;
    DeviceWatcher Watcher;
    std::vector<DeviceWatcherFact> Facts;
};

void TestStartStopRestartRejectsStaleCallbacks() {
    Fixture fixture;
    Check(fixture.Watcher.Start(), "the first start must create and start a platform watcher");
    auto const first = fixture.PlatformAccess->LastWatcher;
    Check(first && first->StartCalls == 1, "start must be delegated exactly once to the platform watcher");

    first->Add(L"z", L"Zulu");
    fixture.Executor.RunAll();
    Check(fixture.Watcher.Snapshot().Devices.size() == 1, "accepted callbacks must populate the normalized inventory");

    fixture.Watcher.Stop();
    Check(first->StopCalls == 1 && first->RevokeCalls == 1, "stop must stop and revoke the active platform watcher");
    auto const stoppedGeneration = fixture.Watcher.Generation();
    first->Add(L"late", L"Late");
    fixture.Executor.RunAll();
    Check(fixture.Watcher.Generation() == stoppedGeneration && fixture.Watcher.Snapshot().Devices.size() == 1,
          "callbacks after stop must not mutate state");

    Check(fixture.Watcher.Start(), "a stopped watcher must restart with a new generation");
    auto const second = fixture.PlatformAccess->LastWatcher;
    Check(second != first, "restart must create a new platform watcher registration");
    Check(fixture.Watcher.Generation() > stoppedGeneration, "restart must allocate a newer watcher generation");
    first->Add(L"stale", L"Stale");
    second->Add(L"fresh", L"Fresh");
    fixture.Executor.RunAll();
    auto const snapshot = fixture.Watcher.Snapshot();
    Check(snapshot.Devices.size() == 1 && snapshot.Devices.front().Id == L"fresh",
          "a stale generation must not resurrect pre-restart inventory");
}

void TestStartFailureRevokesThePartialRegistration() {
    Fixture fixture;
    fixture.PlatformAccess->FailNextStart = true;

    Check(!fixture.Watcher.Start(), "a platform start failure must remain observable to DeviceService");
    auto const failedWatcher = fixture.PlatformAccess->LastWatcher;
    Check(failedWatcher && failedWatcher->StartCalls == 1 && failedWatcher->StopCalls == 1 &&
              failedWatcher->RevokeCalls == 1,
          "a failed start must stop and revoke the partially created platform registration");
    Check(!fixture.Watcher.IsRunning(), "a failed start must not leave the watcher active");
    if (!failedWatcher) return;

    failedWatcher->Add(L"late", L"Late");
    fixture.Executor.RunAll();
    Check(fixture.Watcher.Snapshot().Devices.empty(),
          "a callback from a failed generation must not mutate the normalized inventory");
}

void TestCallbacksAreOrderedOnTheSerializedExecutor() {
    Fixture fixture;
    Check(fixture.Watcher.Start(), "start must succeed before callback ordering can be observed");
    auto const platformWatcher = fixture.PlatformAccess->LastWatcher;

    platformWatcher->Add(L"b", L"Bravo");
    platformWatcher->Add(L"a", L"Alpha");
    platformWatcher->CompleteEnumeration();
    Check(fixture.Executor.PendingCount() == 3,
          "platform callbacks must enqueue instead of mutating watcher state directly");
    Check(fixture.Watcher.Snapshot().Devices.empty(),
          "queued callbacks must not publish before the serialized context runs");

    fixture.Executor.RunOne();
    Check(fixture.Watcher.Snapshot().Devices.size() == 1, "the first queued callback must run first");
    fixture.Executor.RunAll();
    auto const snapshot = fixture.Watcher.Snapshot();
    Check(snapshot.EnumerationComplete, "enumeration completion must be applied after preceding device callbacks");
    Check(snapshot.Devices.size() == 2 && snapshot.Devices[0].Id == L"a" && snapshot.Devices[1].Id == L"b",
          "inventory snapshots must remain deterministically sorted by name then ID");
}

void TestEnumerationCompletePublishesOncePerGeneration() {
    Fixture fixture;
    Check(fixture.Watcher.Start(), "start must establish an active generation");
    auto const platformWatcher = fixture.PlatformAccess->LastWatcher;
    platformWatcher->CompleteEnumeration();
    platformWatcher->CompleteEnumeration();
    fixture.Executor.RunAll();

    auto const completedFacts = std::count_if(fixture.Facts.begin(), fixture.Facts.end(), [](auto const& fact) {
        return fact.Kind == DeviceWatcherFactKind::EnumerationCompleted;
    });
    Check(completedFacts == 1, "duplicate completion callbacks must be coalesced within one generation");
    Check(fixture.Watcher.Snapshot().EnumerationComplete, "the first completion must mark the snapshot complete");
}

void TestShutdownRevokesCallbacksWithoutRetainingOwner() {
    ManualExecutor executor;
    auto platform = std::make_unique<FakeDeviceWatcherPlatform>();
    auto* const platformAccess = platform.get();
    std::size_t factCount = 0;
    auto watcher =
        std::make_unique<DeviceWatcher>([&executor](DeviceWatcher::Task task) { executor.Post(std::move(task)); },
                                        [&factCount](DeviceWatcherFact const&) { ++factCount; },
                                        std::move(platform));
    Check(watcher->Start(), "start must succeed before shutdown lifetime coverage");
    auto const platformWatcher = platformAccess->LastWatcher;
    platformWatcher->Add(L"queued", L"Queued");
    watcher.reset();

    Check(platformWatcher->StopCalls == 1 && platformWatcher->RevokeCalls == 1,
          "destruction must stop and revoke callbacks before releasing state");
    auto const factsBeforeQueuedCallback = factCount;
    executor.RunAll();
    Check(factCount == factsBeforeQueuedCallback,
          "queued callbacks after destruction must not retain or access the destroyed watcher owner");
}

} // namespace

int RunDeviceWatcherTests() {
    TestStartStopRestartRejectsStaleCallbacks();
    TestStartFailureRevokesThePartialRegistration();
    TestCallbacksAreOrderedOnTheSerializedExecutor();
    TestEnumerationCompletePublishesOncePerGeneration();
    TestShutdownRevokesCallbacksWithoutRetainingOwner();
    return g_failures;
}

#ifdef APC_DEVICE_WATCHER_STANDALONE
int main() {
    return RunDeviceWatcherTests();
}
#endif
