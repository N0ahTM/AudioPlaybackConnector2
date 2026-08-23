#include <core/DeviceService.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apc::device::DeviceCommandResultKind;
using apc::device::DeviceConnection;
using apc::device::DeviceConnectionPlatform;
using apc::device::DeviceConnectionResult;
using apc::device::DeviceConnectionState;
using apc::device::DeviceFact;
using apc::device::DeviceFactKind;
using apc::device::DeviceLifecycleState;
using apc::device::DeviceService;
using apc::device::DeviceServiceDependencies;
using apc::device::DeviceTimer;
using apc::device::DeviceTimerPlatform;
using apc::device::DeviceWatcherCallbacks;
using apc::device::DeviceWatcherPlatform;
using apc::device::DeviceWatcherRegistration;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class FakeConnectionState {
public:
    [[nodiscard]] std::uint64_t RegisterStateChanged(DeviceConnection::StateChangedHandler handler) {
        StateChanged = std::move(handler);
        return ++LastToken;
    }

    void RevokeStateChanged(std::uint64_t token) noexcept {
        if (token == LastToken) {
            ++RevokeCalls;
            StateChanged = {};
        }
    }

    void Start(DeviceConnection::Completion completion) { StartCompletion = std::move(completion); }
    void Open(DeviceConnection::Completion completion) { OpenCompletion = std::move(completion); }
    void Close(DeviceConnection::CloseCompletion completion) noexcept {
        ++CloseCalls;
        CloseCompletionCallback = std::move(completion);
    }

    void CompleteStart(DeviceConnectionResult result) {
        if (StartCompletion) StartCompletion(result);
    }
    void CompleteOpen(DeviceConnectionResult result) {
        if (OpenCompletion) OpenCompletion(result);
    }
    void CompleteClose() {
        if (CloseCompletionCallback) CloseCompletionCallback();
    }
    void Signal(DeviceConnectionState state) {
        if (StateChanged) StateChanged(state);
    }

    DeviceConnection::StateChangedHandler StateChanged;
    DeviceConnection::Completion StartCompletion;
    DeviceConnection::Completion OpenCompletion;
    DeviceConnection::CloseCompletion CloseCompletionCallback;
    std::uint64_t LastToken = 0;
    int RevokeCalls = 0;
    int CloseCalls = 0;
};

class FakeConnection final : public DeviceConnection {
public:
    explicit FakeConnection(std::shared_ptr<FakeConnectionState> state) : m_state(std::move(state)) {}

    [[nodiscard]] std::uint64_t RegisterStateChanged(StateChangedHandler handler) override {
        return m_state->RegisterStateChanged(std::move(handler));
    }
    void RevokeStateChanged(std::uint64_t token) noexcept override { m_state->RevokeStateChanged(token); }
    void Start(Completion completion) override { m_state->Start(std::move(completion)); }
    void Open(Completion completion) override { m_state->Open(std::move(completion)); }
    void Close(CloseCompletion completion) noexcept override { m_state->Close(std::move(completion)); }

private:
    std::shared_ptr<FakeConnectionState> m_state;
};

class FakeConnectionPlatform final : public DeviceConnectionPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceConnection> Create(std::wstring const&) override {
        auto state = std::make_shared<FakeConnectionState>();
        LastConnection = state.get();
        Connections.push_back(LastConnection);
        States.push_back(std::move(state));
        return std::make_unique<FakeConnection>(States.back());
    }

    FakeConnectionState* LastConnection = nullptr;
    std::vector<FakeConnectionState*> Connections;
    std::vector<std::shared_ptr<FakeConnectionState>> States;
};

class FakeTimer final : public DeviceTimer {
public:
    explicit FakeTimer(DeviceTimerPlatform::Callback callback) : Callback(std::move(callback)) {}
    void Cancel() noexcept override { IsCancelled = true; }
    void FireEvenIfCancelled() {
        if (Callback) Callback();
    }

    DeviceTimerPlatform::Callback Callback;
    bool IsCancelled = false;
};

class FakeTimerPlatform final : public DeviceTimerPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceTimer> Schedule(std::chrono::seconds, Callback callback) override {
        auto timer = std::make_unique<FakeTimer>(std::move(callback));
        LastTimer = timer.get();
        Timers.push_back(LastTimer);
        return timer;
    }

    FakeTimer* LastTimer = nullptr;
    std::vector<FakeTimer*> Timers;
};

class FakeWatcherRegistration final : public DeviceWatcherRegistration {
public:
    explicit FakeWatcherRegistration(DeviceWatcherCallbacks callbacks) : Callbacks(std::move(callbacks)) {}
    void Start() override { ++StartCalls; }
    void Stop() noexcept override { ++StopCalls; }
    void RevokeCallbacks() noexcept override { ++RevokeCalls; }
    void Add(std::wstring id, std::wstring name) { Callbacks.DeviceAdded({std::move(id), std::move(name)}); }

    DeviceWatcherCallbacks Callbacks;
    int StartCalls = 0;
    int StopCalls = 0;
    int RevokeCalls = 0;
};

class FakeWatcherPlatform final : public DeviceWatcherPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceWatcherRegistration>
    CreateDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) override {
        auto watcher = std::make_unique<FakeWatcherRegistration>(std::move(callbacks));
        LastWatcher = watcher.get();
        return watcher;
    }

    FakeWatcherRegistration* LastWatcher = nullptr;
};

struct Fixture {
    Fixture()
        : Connections(std::make_unique<FakeConnectionPlatform>()), Timers(std::make_unique<FakeTimerPlatform>()),
          Watchers(std::make_unique<FakeWatcherPlatform>()), ConnectionAccess(Connections.get()),
          TimerAccess(Timers.get()), WatcherAccess(Watchers.get()),
          Service(DeviceServiceDependencies{.WatcherPlatform = std::move(Watchers),
                                            .ConnectionPlatform = std::move(Connections),
                                            .TimerPlatform = std::move(Timers)}) {
        (void)Service.Subscribe([this](DeviceFact const& fact) { Facts.push_back(fact); });
    }

    std::unique_ptr<FakeConnectionPlatform> Connections;
    std::unique_ptr<FakeTimerPlatform> Timers;
    std::unique_ptr<FakeWatcherPlatform> Watchers;
    FakeConnectionPlatform* ConnectionAccess;
    FakeTimerPlatform* TimerAccess;
    FakeWatcherPlatform* WatcherAccess;
    DeviceService Service;
    std::vector<DeviceFact> Facts;
};

DeviceLifecycleState StateFor(DeviceService const& service, std::wstring_view deviceId) {
    auto const snapshot = service.Snapshot();
    auto const found = std::ranges::find(snapshot.Sessions, deviceId, &apc::device::DeviceSessionSnapshot::DeviceId);
    return found == snapshot.Sessions.end() ? DeviceLifecycleState::Failed : found->State;
}

void ConnectSuccessfully(Fixture& fixture, std::wstring id) {
    (void)fixture.Service.Connect(std::move(id));
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
}

void TestOperationEpochRejectsStaleCompletion() {
    Fixture fixture;
    (void)fixture.Service.Connect(L"epoch");
    auto* const first = fixture.ConnectionAccess->LastConnection;
    (void)fixture.Service.Disconnect(L"epoch");
    first->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"epoch") == DeviceLifecycleState::Disconnecting,
          "a stale start completion must not reopen a disconnecting session");
    first->CompleteClose();
    Check(StateFor(fixture.Service, L"epoch") == DeviceLifecycleState::Idle,
          "the current close completion must settle the session to idle");
}

void TestReconnectWaitsForCloseAndRevokesTheOldToken() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"barrier");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();
    (void)fixture.Service.Reconnect(L"barrier");
    Check(oldConnection->RevokeCalls == 1 && oldConnection->CloseCalls == 1,
          "reconnect must revoke the old state token before closing it");
    Check(fixture.ConnectionAccess->Connections.size() == createCount,
          "a replacement must not be created before the close barrier completes");
    oldConnection->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == createCount + 1,
          "the close completion must be the only transition that starts replacement creation");
}

void TestIncomingConnectionDoesNotOpenUntilThePlatformSignalsOpened() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"incoming", L"Incoming");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    connection->CompleteStart(DeviceConnectionResult::Success);
    Check(!connection->OpenCompletion, "incoming enablement must start listening without issuing OpenAsync");
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Idle,
          "an incoming listener is idle until an inbound connection opens");
    connection->Signal(DeviceConnectionState::Opened);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Connected,
          "a platform Opened callback must transition the retained listener to connected");
}

void TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"retry");
    auto* const first = fixture.ConnectionAccess->LastConnection;
    first->Signal(DeviceConnectionState::Closed);
    auto* const timer = fixture.TimerAccess->LastTimer;
    Check(StateFor(fixture.Service, L"retry") == DeviceLifecycleState::WaitingForReconnect,
          "unexpected loss must schedule the bounded reconnect policy");
    (void)fixture.Service.CancelReconnect(L"retry");
    timer->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == 1,
          "a cancelled timer callback must not create a replacement connection");
}

void TestBulkSuspendResumeAndShutdownCannotResurrectSessions() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"a");
    ConnectSuccessfully(fixture, L"b");
    (void)fixture.Service.DisconnectAll();
    for (auto* connection : fixture.ConnectionAccess->Connections)
        connection->CompleteClose();
    Check(StateFor(fixture.Service, L"a") == DeviceLifecycleState::Idle &&
              StateFor(fixture.Service, L"b") == DeviceLifecycleState::Idle,
          "bulk disconnect must settle every serialized session");

    (void)fixture.Service.Connect(L"resume");
    auto* const busy = fixture.ConnectionAccess->LastConnection;
    fixture.Service.Suspend();
    busy->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"resume") == DeviceLifecycleState::Idle,
          "suspend must invalidate in-flight completions before they can mutate state");
    fixture.Service.Resume();
    Check(fixture.ConnectionAccess->Connections.size() >= 4, "resume must restart a previously busy connection target");

    auto* const late = fixture.ConnectionAccess->LastConnection;
    fixture.Service.Shutdown();
    late->CompleteStart(DeviceConnectionResult::Success);
    late->Signal(DeviceConnectionState::Opened);
    Check(fixture.Service.Snapshot().IsShutdown && fixture.Service.Snapshot().Sessions.empty(),
          "shutdown must release sessions and reject every late platform callback");
}

void TestFactsCarryNormalizedSnapshots() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"z");
    ConnectSuccessfully(fixture, L"a");
    auto const hasSortedSnapshot = std::ranges::any_of(fixture.Facts, [](DeviceFact const& fact) {
        return fact.Kind == DeviceFactKind::SessionChanged && fact.Snapshot.Sessions.size() == 2 &&
               fact.Snapshot.Sessions[0].DeviceId == L"a" && fact.Snapshot.Sessions[1].DeviceId == L"z";
    });
    Check(hasSortedSnapshot, "typed facts must contain normalized, deterministically ordered snapshots");
}

} // namespace

int RunDeviceServiceTests() {
    TestOperationEpochRejectsStaleCompletion();
    TestReconnectWaitsForCloseAndRevokesTheOldToken();
    TestIncomingConnectionDoesNotOpenUntilThePlatformSignalsOpened();
    TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks();
    TestBulkSuspendResumeAndShutdownCannotResurrectSessions();
    TestFactsCarryNormalizedSnapshots();
    return g_failures;
}

#ifdef APC_DEVICE_SERVICE_STANDALONE
int main() {
    return RunDeviceServiceTests();
}
#endif
