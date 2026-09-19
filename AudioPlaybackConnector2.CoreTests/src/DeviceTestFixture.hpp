#pragma once

#include <app/AppController.hpp>
#include <core/DeviceService.hpp>

#include <winerror.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <iostream>
#include <future>
#include <semaphore>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace apc::tests::device {
using apc::device::DeviceCommandKind;
using apc::device::DeviceCommandResultKind;
using apc::device::DeviceConnection;
using apc::device::DeviceConnectionPlatform;
using apc::device::DeviceConnectionResult;
using apc::device::DeviceConnectionState;
using apc::device::DeviceDisconnectReason;
using apc::device::DeviceFact;
using apc::device::DeviceFactKind;
using apc::device::DeviceLifecycleState;
using apc::device::DeviceOpenResult;
using apc::device::DeviceOperationStatus;
using apc::device::DeviceService;
using apc::device::DeviceServiceDependencies;
using apc::device::DeviceTimer;
using apc::device::DeviceTimerPlatform;
using apc::device::DeviceWatcherCallbacks;
using apc::device::DeviceWatcherPlatform;
using apc::device::DeviceWatcherRegistration;

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
    void Open(DeviceConnection::OpenCompletion completion) {
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
        if (OpenCompletion) OpenCompletion({.Result = result});
    }
    void CompleteOpen(DeviceOpenResult result) {
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
    DeviceConnection::OpenCompletion OpenCompletion;
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
    void Open(OpenCompletion completion) override { m_state->Open(std::move(completion)); }
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
    FakeTimer(std::chrono::milliseconds delay, DeviceTimerPlatform::Callback callback)
        : Delay(delay), Callback(std::move(callback)) {}
    void Cancel() noexcept override {
        IsCancelled = true;
        *CancellationState = true;
    }
    void FireEvenIfCancelled() {
        if (Callback) Callback();
    }

    std::chrono::milliseconds Delay;
    DeviceTimerPlatform::Callback Callback;
    bool IsCancelled = false;
    std::shared_ptr<bool> CancellationState = std::make_shared<bool>(false);
};

class FakeTimerPlatform final : public DeviceTimerPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceTimer> Schedule(std::chrono::milliseconds delay, Callback callback) override {
        if (ThrowNextSchedule) {
            ThrowNextSchedule = false;
            throw std::runtime_error("Schedule");
        }
        if (ReturnNullNextSchedule) {
            ReturnNullNextSchedule = false;
            return nullptr;
        }
        auto timer = std::make_unique<FakeTimer>(delay, std::move(callback));
        LastTimer = timer.get();
        Timers.push_back(LastTimer);
        return timer;
    }

    FakeTimer* LastTimer = nullptr;
    std::vector<FakeTimer*> Timers;
    bool ThrowNextSchedule = false;
    bool ReturnNullNextSchedule = false;
};

class FakeWatcherState {
public:
    explicit FakeWatcherState(DeviceWatcherCallbacks callbacks) : Callbacks(std::move(callbacks)) {}

    void Start() {
        ++StartCalls;
        if (FailStart) throw std::runtime_error("Start watcher");
    }
    void Stop() noexcept { ++StopCalls; }
    void RevokeCallbacks() noexcept { ++RevokeCalls; }
    void Add(std::wstring id, std::wstring name) { Callbacks.DeviceAdded({std::move(id), std::move(name)}); }
    void Remove(std::wstring id) { Callbacks.DeviceRemoved(std::move(id)); }

    DeviceWatcherCallbacks Callbacks;
    int StartCalls = 0;
    int StopCalls = 0;
    int RevokeCalls = 0;
    bool FailStart = false;
};

class FakeWatcherRegistration final : public DeviceWatcherRegistration {
public:
    explicit FakeWatcherRegistration(std::shared_ptr<FakeWatcherState> state) : m_state(std::move(state)) {}

    void Start() override { m_state->Start(); }
    void Stop() noexcept override { m_state->Stop(); }
    void RevokeCallbacks() noexcept override { m_state->RevokeCallbacks(); }

private:
    std::shared_ptr<FakeWatcherState> m_state;
};

class FakeWatcherPlatform final : public DeviceWatcherPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceWatcherRegistration>
    CreateDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) override {
        auto watcher = std::make_shared<FakeWatcherState>(std::move(callbacks));
        watcher->FailStart = FailNextStart;
        FailNextStart = false;
        LastWatcher = watcher.get();
        Watchers.push_back(std::move(watcher));
        return std::make_unique<FakeWatcherRegistration>(Watchers.back());
    }

    winrt::Windows::Foundation::IAsyncAction RefreshAsync(RefreshCompletion completion) override {
        if (BeforeRefreshCompletion) BeforeRefreshCompletion();
        if (completion) completion(nullptr, {});
        co_return;
    }

    std::function<void()> BeforeRefreshCompletion;
    FakeWatcherState* LastWatcher = nullptr;
    std::vector<std::shared_ptr<FakeWatcherState>> Watchers;
    bool FailNextStart = false;
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

    ~Fixture() { Service.Shutdown(); }

    std::unique_ptr<FakeConnectionPlatform> Connections;
    std::unique_ptr<FakeTimerPlatform> Timers;
    std::unique_ptr<FakeWatcherPlatform> Watchers;
    FakeConnectionPlatform* ConnectionAccess;
    FakeTimerPlatform* TimerAccess;
    FakeWatcherPlatform* WatcherAccess;
    DeviceService Service;
    std::vector<DeviceFact> Facts;
};

inline DeviceLifecycleState StateFor(DeviceService const& service, std::wstring_view deviceId) {
    auto const snapshot = service.Snapshot();
    auto const found = std::ranges::find(snapshot.Sessions, deviceId, &apc::device::DeviceSessionSnapshot::DeviceId);
    return found == snapshot.Sessions.end() ? DeviceLifecycleState::Failed : found->State;
}

inline apc::device::DeviceSessionSnapshot SessionFor(DeviceService const& service, std::wstring_view deviceId) {
    auto const snapshot = service.Snapshot();
    auto const found = std::ranges::find(snapshot.Sessions, deviceId, &apc::device::DeviceSessionSnapshot::DeviceId);
    return found == snapshot.Sessions.end() ? apc::device::DeviceSessionSnapshot{} : *found;
}

inline void ConnectSuccessfully(Fixture& fixture, std::wstring id) {
    (void)fixture.Service.Connect(std::move(id));
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
}

inline void CompleteCloseAndCooldown(Fixture& fixture, FakeConnectionState* connection) {
    connection->CompleteClose();
    auto* const cooldown = fixture.TimerAccess->LastTimer;
    if (cooldown && cooldown->Delay == std::chrono::milliseconds(1500)) cooldown->FireEvenIfCancelled();
}

} // namespace apc::tests::device
