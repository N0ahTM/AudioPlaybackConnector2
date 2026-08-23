#include <core/DeviceService.hpp>

#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using apc::device::DeviceCommandKind;
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

struct FakeConnectionBehavior {
    bool ThrowOnRegister = false;
    bool ThrowOnStart = false;
    bool ThrowOnOpen = false;
};

class FakeConnectionState {
public:
    explicit FakeConnectionState(FakeConnectionBehavior behavior) : Behavior(behavior) {}

    [[nodiscard]] std::uint64_t RegisterStateChanged(DeviceConnection::StateChangedHandler handler) {
        if (Behavior.ThrowOnRegister) throw std::runtime_error("RegisterStateChanged");
        StateChanged = std::move(handler);
        return ++LastToken;
    }

    void RevokeStateChanged(std::uint64_t token) noexcept {
        if (token == LastToken) {
            ++RevokeCalls;
            StateChanged = {};
        }
    }

    void Start(DeviceConnection::Completion completion) {
        if (Behavior.ThrowOnStart) throw std::runtime_error("Start");
        StartCompletion = std::move(completion);
    }
    void Open(DeviceConnection::Completion completion) {
        if (Behavior.ThrowOnOpen) throw std::runtime_error("Open");
        OpenCompletion = std::move(completion);
    }
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
    FakeConnectionBehavior Behavior;
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
        auto state = std::make_shared<FakeConnectionState>(NextBehavior);
        LastConnection = state.get();
        Connections.push_back(LastConnection);
        States.push_back(std::move(state));
        return std::make_unique<FakeConnection>(States.back());
    }

    FakeConnectionState* LastConnection = nullptr;
    std::vector<FakeConnectionState*> Connections;
    std::vector<std::shared_ptr<FakeConnectionState>> States;
    FakeConnectionBehavior NextBehavior;
};

class FakeTimer final : public DeviceTimer {
public:
    FakeTimer(std::chrono::seconds delay, DeviceTimerPlatform::Callback callback)
        : Delay(delay), Callback(std::move(callback)) {}
    void Cancel() noexcept override { IsCancelled = true; }
    void FireEvenIfCancelled() {
        if (Callback) Callback();
    }

    std::chrono::seconds Delay;
    DeviceTimerPlatform::Callback Callback;
    bool IsCancelled = false;
};

class FakeTimerPlatform final : public DeviceTimerPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceTimer> Schedule(std::chrono::seconds delay, Callback callback) override {
        if (ThrowNextSchedule) {
            ThrowNextSchedule = false;
            throw std::runtime_error("Schedule");
        }
        auto timer = std::make_unique<FakeTimer>(delay, std::move(callback));
        LastTimer = timer.get();
        Timers.push_back(LastTimer);
        return timer;
    }

    FakeTimer* LastTimer = nullptr;
    std::vector<FakeTimer*> Timers;
    bool ThrowNextSchedule = false;
};

class FakeWatcherRegistration final : public DeviceWatcherRegistration {
public:
    explicit FakeWatcherRegistration(DeviceWatcherCallbacks callbacks) : Callbacks(std::move(callbacks)) {}
    void Start() override { ++StartCalls; }
    void Stop() noexcept override { ++StopCalls; }
    void RevokeCallbacks() noexcept override { ++RevokeCalls; }
    void Add(std::wstring id, std::wstring name) { Callbacks.DeviceAdded({std::move(id), std::move(name)}); }
    void Remove(std::wstring id) { Callbacks.DeviceRemoved(std::move(id)); }

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

apc::device::DeviceSessionSnapshot SessionFor(DeviceService const& service, std::wstring_view deviceId) {
    auto const snapshot = service.Snapshot();
    auto const found = std::ranges::find(snapshot.Sessions, deviceId, &apc::device::DeviceSessionSnapshot::DeviceId);
    return found == snapshot.Sessions.end() ? apc::device::DeviceSessionSnapshot{} : *found;
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

void TestCloseBarrierIsOneShotAndFallsBackAfterBoundedTimeout() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"close-timeout");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();

    (void)fixture.Service.Reconnect(L"close-timeout");
    (void)fixture.Service.Reconnect(L"close-timeout");
    auto* const closeBarrierTimer = fixture.TimerAccess->LastTimer;
    Check(oldConnection->CloseCalls == 1, "a close barrier must issue exactly one close while it is in flight");
    Check(closeBarrierTimer->Delay == std::chrono::seconds(5), "the close barrier must retain a bounded cooldown");
    closeBarrierTimer->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == createCount + 1,
          "the close timeout fallback must release exactly one replacement operation");
    oldConnection->CompleteClose();
    Check(oldConnection->CloseCalls == 1 && fixture.ConnectionAccess->Connections.size() == createCount + 1,
          "late close completion must neither close again nor create another replacement");
    Check(std::ranges::any_of(fixture.Facts,
                              [](DeviceFact const& fact) {
                                  return fact.DeviceId == L"close-timeout" && fact.IsTerminalFailure &&
                                         fact.ConnectionResult == DeviceConnectionResult::TimedOut;
                              }),
          "the close timeout fallback must publish a deterministic terminal timeout fact");
}

void TestIncomingCallbackOrderingAndLossFollowReconnectPolicy() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"incoming", L"Incoming");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    connection->Signal(DeviceConnectionState::Opened);
    connection->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Connected,
          "a delayed incoming Start completion must not overwrite an established Connected state");
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();
    connection->Signal(DeviceConnectionState::Closed);
    auto* const retryTimer = fixture.TimerAccess->LastTimer;
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount &&
              StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::WaitingForReconnect,
          "an established incoming loss must enter the configured reconnect policy");
    retryTimer->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
          "the incoming reconnect timer must recreate a listener only after its policy delay");
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Idle,
          "an incoming reconnect must return to listening state without OpenAsync");

    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Opened);
    fixture.Service.ConfigureReconnectPolicy(false, {});
    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Failed,
          "an established incoming loss must respect disabled reconnect policy");
}

void TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.WatcherAccess->LastWatcher->Add(L"removed", L"Removed");
    ConnectSuccessfully(fixture, L"removed");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    const auto connectionCount = fixture.ConnectionAccess->Connections.size();

    fixture.WatcherAccess->LastWatcher->Remove(L"removed");
    Check(connection->CloseCalls == 1, "device removal must close the retained connection exactly once");
    connection->CompleteStart(DeviceConnectionResult::Success);
    connection->Signal(DeviceConnectionState::Opened);
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount,
          "late callbacks from a removed device must not create a replacement connection");
    connection->CompleteClose();
    Check(StateFor(fixture.Service, L"removed") == DeviceLifecycleState::WaitingForReconnect &&
              !SessionFor(fixture.Service, L"removed").IsReconnectCancelled,
          "device removal must preserve loss recovery without recording explicit user cancellation");

    fixture.WatcherAccess->LastWatcher->Add(L"removed", L"Removed Again");
    Check(SessionFor(fixture.Service, L"removed").DeviceName == L"Removed Again",
          "reappearing devices must update the retained session snapshot");
    fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"removed") == DeviceLifecycleState::Connected,
          "a returned device must recover through the retained reconnect policy");
}

void TestPlatformSetupExceptionsCleanUpAndPublishTerminalFacts() {
    auto checkFailure = [](Fixture& fixture, std::wstring_view deviceId, std::string_view context) {
        auto const session = SessionFor(fixture.Service, deviceId);
        Check(session.State == DeviceLifecycleState::Failed && !session.HasConnection,
              "a platform setup exception must leave no retained connecting connection");
        Check(std::ranges::any_of(fixture.Facts,
                                  [deviceId](DeviceFact const& fact) {
                                      return fact.DeviceId == deviceId && fact.IsTerminalFailure &&
                                             fact.ConnectionResult == DeviceConnectionResult::Failed;
                                  }),
              context);
    };

    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnRegister = true;
        (void)fixture.Service.Connect(L"register-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        Check(connection->CloseCalls == 1, "handler registration failure must close the created connection once");
        connection->CompleteClose();
        checkFailure(fixture, L"register-throws", "handler registration failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
        (void)fixture.Service.Connect(L"start-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        Check(connection->RevokeCalls == 1 && connection->CloseCalls == 1,
              "Start failure must revoke its token and close the created connection once");
        connection->CompleteClose();
        checkFailure(fixture, L"start-throws", "Start failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnOpen = true;
        (void)fixture.Service.Connect(L"open-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        Check(connection->RevokeCalls == 1 && connection->CloseCalls == 1,
              "Open failure must revoke its token and close the created connection once");
        connection->CompleteClose();
        checkFailure(fixture, L"open-throws", "Open failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"retry-timer-throws");
        fixture.TimerAccess->ThrowNextSchedule = true;
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        checkFailure(
            fixture, L"retry-timer-throws", "reconnect timer setup failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"close-timer-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const createCount = fixture.ConnectionAccess->Connections.size();
        fixture.TimerAccess->ThrowNextSchedule = true;
        (void)fixture.Service.Reconnect(L"close-timer-throws");
        Check(connection->CloseCalls == 1 && fixture.ConnectionAccess->Connections.size() == createCount + 1,
              "close timer setup failure must release its close barrier through the deterministic fallback");
        connection->CompleteClose();
        Check(fixture.ConnectionAccess->Connections.size() == createCount + 1,
              "a late close completion after timer setup failure must remain stale");
        Check(std::ranges::any_of(fixture.Facts,
                                  [](DeviceFact const& fact) {
                                      return fact.DeviceId == L"close-timer-throws" && fact.IsTerminalFailure &&
                                             fact.ConnectionResult == DeviceConnectionResult::Failed;
                                  }),
              "close timer setup failure must publish a terminal failure fact");
    }
}

void TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"retry");
    auto* const first = fixture.ConnectionAccess->LastConnection;
    first->Signal(DeviceConnectionState::Closed);
    auto* timer = fixture.TimerAccess->LastTimer;
    Check(StateFor(fixture.Service, L"retry") == DeviceLifecycleState::WaitingForReconnect,
          "unexpected loss must schedule the bounded reconnect policy");
    Check(timer->Delay == std::chrono::seconds(5) && SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 0,
          "the initial reconnect timer must be attempt one at five seconds without consuming a completed failure");
    auto firstTimerCallback = timer->Callback;
    if (firstTimerCallback) firstTimerCallback();
    auto* const retry = fixture.ConnectionAccess->LastConnection;
    retry->CompleteStart(DeviceConnectionResult::Success);
    retry->CompleteOpen(DeviceConnectionResult::Failed);
    retry->CompleteClose();
    Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::seconds(10) &&
              SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 1,
          "only a completed automatic attempt may advance the retry count and backoff");
    timer = fixture.TimerAccess->LastTimer;
    auto staleTimerCallback = timer->Callback;
    (void)fixture.Service.CancelReconnect(L"retry");
    if (staleTimerCallback) staleTimerCallback();
    Check(fixture.ConnectionAccess->Connections.size() == 2 &&
              SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 1,
          "a cancelled pending timer must not create or count another automatic attempt");
}

void TestReconnectPolicyAndUserCancellationRemainDistinct() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"policy");

    fixture.Service.ConfigureReconnectPolicy(false, {});
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "disabling reconnect policy must be observable independently of user cancellation");
    fixture.Service.ConfigureReconnectPolicy(true, {L"policy"});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "re-enabling reconnect policy must allow later connection-loss retries");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not be recorded as user cancellation");

    (void)fixture.Service.CancelReconnect(L"policy");
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "manual reconnect cancellation must remain observable as user state");
    fixture.Service.ConfigureReconnectPolicy(false, {});
    fixture.Service.ConfigureReconnectPolicy(true, {L"policy"});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not erase an explicit user cancellation");

    (void)fixture.Service.Connect(L"policy");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "a later manual connect must explicitly clear user cancellation");
}

void TestConcurrentCommandWaitsForSerializedMutation() {
    Fixture fixture;
    std::mutex gateMutex;
    std::condition_variable gate;
    bool factSinkEntered = false;
    bool releaseFactSink = false;

    (void)fixture.Service.Subscribe([&](DeviceFact const& fact) {
        if (fact.Kind != DeviceFactKind::InventoryChanged) return;
        std::unique_lock lock(gateMutex);
        if (factSinkEntered) return;
        factSinkEntered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return releaseFactSink; });
    });

    apc::device::DeviceCommandResult concurrentResult;
    std::jthread concurrentCaller([&] {
        {
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [&] { return factSinkEntered; });
        }
        concurrentResult = fixture.Service.Connect(L"concurrent");
    });
    std::jthread releaseCaller([&] {
        {
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [&] { return factSinkEntered; });
        }
        {
            std::lock_guard lock(gateMutex);
            releaseFactSink = true;
        }
        gate.notify_all();
    });

    const auto startResult = fixture.Service.Start();
    concurrentCaller.join();
    releaseCaller.join();

    Check(startResult.Kind == DeviceCommandResultKind::Accepted,
          "the first serialized command must complete before a concurrent command");
    Check(concurrentResult.Kind == DeviceCommandResultKind::Accepted && concurrentResult.DeviceId == L"concurrent",
          "a concurrent command must wait for the serialized context and receive its actual result");
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

    ConnectSuccessfully(fixture, L"resume");
    auto* const busy = fixture.ConnectionAccess->LastConnection;
    auto const beforeResume = fixture.ConnectionAccess->Connections.size();
    fixture.Service.Suspend();
    fixture.Service.Resume();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume,
          "resume must not create a replacement before the suspend close barrier completes");
    busy->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
          "the current suspend close completion must release the resume replacement");
    busy->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
          "a stale suspend close completion must not create another replacement");

    auto* const late = fixture.ConnectionAccess->LastConnection;
    fixture.Service.Shutdown();
    late->CompleteStart(DeviceConnectionResult::Success);
    late->Signal(DeviceConnectionState::Opened);
    Check(fixture.Service.Snapshot().IsShutdown && fixture.Service.Snapshot().Sessions.empty(),
          "shutdown must release sessions and reject every late platform callback");
}

void TestStopAndShutdownReturnNormalizedTerminalResults() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "start must establish watcher ownership");
    auto const stop = fixture.Service.Stop();
    Check(stop.Command == DeviceCommandKind::Stop && stop.Kind == DeviceCommandResultKind::Accepted,
          "stop must report the Stop command kind rather than Start");
    fixture.Service.Shutdown();
    auto const snapshot = fixture.Service.Snapshot();
    Check(snapshot.IsShutdown && snapshot.Sessions.empty() && !snapshot.IsRunning,
          "post-shutdown snapshots must retain the terminal shutdown state");
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
    TestCloseBarrierIsOneShotAndFallsBackAfterBoundedTimeout();
    TestIncomingCallbackOrderingAndLossFollowReconnectPolicy();
    TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks();
    TestPlatformSetupExceptionsCleanUpAndPublishTerminalFacts();
    TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks();
    TestReconnectPolicyAndUserCancellationRemainDistinct();
    TestConcurrentCommandWaitsForSerializedMutation();
    TestBulkSuspendResumeAndShutdownCannotResurrectSessions();
    TestStopAndShutdownReturnNormalizedTerminalResults();
    TestFactsCarryNormalizedSnapshots();
    return g_failures;
}

#ifdef APC_DEVICE_SERVICE_STANDALONE
int main() {
    return RunDeviceServiceTests();
}
#endif
