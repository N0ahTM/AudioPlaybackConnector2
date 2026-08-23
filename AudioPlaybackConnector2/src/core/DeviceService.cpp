#include <pch.h>

#include <core/DeviceService.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace apc::device {

struct DeviceService::State : std::enable_shared_from_this<DeviceService::State> {
    using Task = std::function<void()>;

    struct Completion {
        std::mutex Mutex;
        std::condition_variable Condition;
        bool Done = false;

        void Signal() noexcept {
            {
                std::lock_guard guard(Mutex);
                Done = true;
            }
            Condition.notify_all();
        }

        void Wait() {
            std::unique_lock lock(Mutex);
            Condition.wait(lock, [this] { return Done; });
        }
    };

    struct QueuedTask {
        Task Work;
        std::shared_ptr<Completion> CompletionState;
    };

    std::mutex QueueMutex;
    std::deque<QueuedTask> Queue;
    bool IsExecuting = false;
    std::thread::id ExecutorThread;

    mutable std::mutex SnapshotMutex;
    DeviceServiceSnapshot PublishedSnapshot;

    std::unique_ptr<DeviceWatcherPlatform> WatcherPlatform;
    std::unique_ptr<DeviceConnectionPlatform> ConnectionPlatform;
    std::unique_ptr<DeviceTimerPlatform> TimerPlatform;
    std::unique_ptr<DeviceWatcher> Watcher;
    std::unordered_map<std::wstring, std::shared_ptr<DeviceSession>> Sessions;
    std::unordered_set<std::wstring> IndividuallyReconnectEnabled;
    FactSink Subscriber;
    Subscription ActiveSubscription = 0;
    Subscription NextSubscription = 1;
    std::uint64_t Generation = 0;
    bool IsRunning = false;
    bool IsSuspended = false;
    bool IsShutdown = false;
    bool IsIncomingEnabled = false;
    bool IsGlobalReconnectEnabled = true;

    void Initialize() {
        if (!ConnectionPlatform) ConnectionPlatform = CreateWindowsDeviceConnectionPlatform();
        if (!TimerPlatform) TimerPlatform = CreateWindowsDeviceTimerPlatform();
        auto weak = weak_from_this();
        Watcher = std::make_unique<DeviceWatcher>(
            [weak](DeviceWatcher::Task task) {
                if (auto service = weak.lock()) static_cast<void>(service->Post(std::move(task)));
            },
            [weak](DeviceWatcherFact const& fact) {
                if (auto service = weak.lock()) service->OnWatcherFact(fact);
            },
            std::move(WatcherPlatform));
        UpdatePublishedSnapshot();
    }

    // The queue lock protects queue bookkeeping only. Tasks, platform calls, and fact publication run after it is
    // released.
    [[nodiscard]] bool Post(Task task) {
        bool runsHere = false;
        bool waitsForCompletion = false;
        auto completion = std::make_shared<Completion>();
        {
            std::lock_guard guard(QueueMutex);
            Queue.push_back({.Work = std::move(task), .CompletionState = completion});
            if (!IsExecuting) {
                IsExecuting = true;
                ExecutorThread = std::this_thread::get_id();
                runsHere = true;
            } else if (ExecutorThread != std::this_thread::get_id()) {
                waitsForCompletion = true;
            }
        }
        if (!runsHere) {
            if (waitsForCompletion) {
                completion->Wait();
                return true;
            }
            return false;
        }

        for (;;) {
            QueuedTask next;
            {
                std::lock_guard guard(QueueMutex);
                if (Queue.empty()) {
                    IsExecuting = false;
                    ExecutorThread = {};
                    break;
                }
                next = std::move(Queue.front());
                Queue.pop_front();
            }
            try {
                next.Work();
            } catch (...) {
                util::DebugTraceUnknownException(L"[DeviceService] serialized task failed");
            }
            next.CompletionState->Signal();
        }
        return true;
    }

    [[nodiscard]] DeviceServiceSnapshot Snapshot() const {
        std::lock_guard guard(SnapshotMutex);
        return PublishedSnapshot;
    }

    void UpdatePublishedSnapshot() {
        DeviceServiceSnapshot snapshot;
        snapshot.Generation = Generation;
        snapshot.IsRunning = IsRunning;
        snapshot.IsSuspended = IsSuspended;
        snapshot.IsShutdown = IsShutdown;
        if (Watcher) snapshot.Inventory = Watcher->Snapshot();
        snapshot.Sessions.reserve(Sessions.size());
        for (auto const& [id, session] : Sessions) {
            (void)id;
            snapshot.Sessions.push_back(session->Snapshot());
        }
        std::ranges::sort(snapshot.Sessions, {}, &DeviceSessionSnapshot::DeviceId);
        std::lock_guard guard(SnapshotMutex);
        PublishedSnapshot = std::move(snapshot);
    }

    void Publish(DeviceFactKind kind,
                 std::wstring deviceId = {},
                 DeviceConnectionResult result = DeviceConnectionResult::Success,
                 bool terminal = false) {
        ++Generation;
        UpdatePublishedSnapshot();
        if (Subscriber)
            Subscriber({.Kind = kind,
                        .Snapshot = Snapshot(),
                        .DeviceId = std::move(deviceId),
                        .ConnectionResult = result,
                        .IsTerminalFailure = terminal});
    }

    [[nodiscard]] std::wstring DeviceName(std::wstring const& deviceId) const {
        if (!Watcher) return {};
        auto const inventory = Watcher->Snapshot();
        auto const found = std::ranges::find(inventory.Devices, deviceId, &device_picker::DeviceIdentity::Id);
        return found == inventory.Devices.end() ? std::wstring{} : found->Name;
    }

    [[nodiscard]] std::shared_ptr<DeviceSession> GetOrCreateSession(std::wstring const& deviceId) {
        if (deviceId.empty() || IsShutdown) return {};
        if (auto existing = Sessions.find(deviceId); existing != Sessions.end()) return existing->second;

        auto weak = weak_from_this();
        auto session = std::make_shared<DeviceSession>(
            deviceId,
            DeviceName(deviceId),
            [weak](DeviceSession::Task task) {
                if (auto service = weak.lock()) static_cast<void>(service->Post(std::move(task)));
            },
            *ConnectionPlatform,
            *TimerPlatform,
            [weak](DeviceSessionFact const& fact) {
                if (auto service = weak.lock()) service->OnSessionFact(fact);
            });
        Sessions.emplace(deviceId, session);
        session->SetReconnectEnabled(IsGlobalReconnectEnabled || IndividuallyReconnectEnabled.contains(deviceId));
        if (IsIncomingEnabled) session->SetIncomingEnabled(true);
        return session;
    }

    void OnSessionFact(DeviceSessionFact const& fact) {
        if (IsShutdown) return;
        auto const isFailure = fact.Result == DeviceConnectionResult::TimedOut ||
                               fact.Result == DeviceConnectionResult::Denied ||
                               fact.Result == DeviceConnectionResult::Failed;
        Publish(isFailure ? DeviceFactKind::OperationFailed : DeviceFactKind::SessionChanged,
                fact.Snapshot.DeviceId,
                fact.Result,
                fact.IsTerminalFailure);
    }

    void OnWatcherFact(DeviceWatcherFact const& fact) {
        if (IsShutdown) return;
        if (fact.Kind == DeviceWatcherFactKind::DeviceAdded) {
            auto session = GetOrCreateSession(fact.DeviceId);
            if (session) {
                session->Rename(fact.DeviceName);
                if (IsIncomingEnabled) session->SetIncomingEnabled(true);
            }
        } else if (fact.Kind == DeviceWatcherFactKind::DeviceRemoved) {
            if (auto existing = Sessions.find(fact.DeviceId); existing != Sessions.end()) {
                existing->second->HandleDeviceRemoved();
            }
        }
        Publish(DeviceFactKind::InventoryChanged, fact.DeviceId);
    }

    [[nodiscard]] DeviceCommandResult
    Result(DeviceCommandKind command, DeviceCommandResultKind kind, std::wstring deviceId = {}) const {
        DeviceCommandResult result;
        result.Command = command;
        result.Kind = kind;
        result.DeviceId = std::move(deviceId);
        if (!result.DeviceId.empty()) {
            if (auto existing = Sessions.find(result.DeviceId); existing != Sessions.end()) {
                result.OperationEpoch = existing->second->Snapshot().OperationEpoch;
            }
        }
        return result;
    }

    void StopAndReleaseSessions() noexcept {
        if (Watcher) Watcher->Shutdown();
        for (auto const& [id, session] : Sessions) {
            (void)id;
            session->Shutdown();
        }
        Sessions.clear();
    }

    void CancelOperation(std::wstring const& deviceId, std::uint64_t operationEpoch) {
        if (IsShutdown) return;
        auto const iter = Sessions.find(deviceId);
        if (iter == Sessions.end() || iter->second->Snapshot().OperationEpoch != operationEpoch) return;
        iter->second->CancelReconnect();
        iter->second->Disconnect(false);
    }

    winrt::Windows::Foundation::IAsyncAction
    AwaitTerminal(std::wstring deviceId, std::uint64_t operationEpoch, bool ownsCancellation) {
        auto const cancellation = co_await winrt::get_cancellation_token();
        auto const weak = weak_from_this();
        if (ownsCancellation) {
            cancellation.callback([weak, deviceId, operationEpoch] {
                if (auto state = weak.lock()) {
                    static_cast<void>(state->Post(
                        [state, deviceId, operationEpoch] { state->CancelOperation(deviceId, operationEpoch); }));
                }
            });
        }

        for (;;) {
            if (cancellation()) co_return;
            auto const snapshot = Snapshot();
            if (snapshot.IsShutdown) throw winrt::hresult_error(E_ABORT);

            auto const iter = std::ranges::find(snapshot.Sessions, deviceId, &DeviceSessionSnapshot::DeviceId);
            if (iter == snapshot.Sessions.end() || iter->OperationEpoch != operationEpoch) {
                throw winrt::hresult_error(E_ABORT);
            }
            if (iter->State == DeviceLifecycleState::Connected) co_return;
            if (iter->State == DeviceLifecycleState::Failed) throw winrt::hresult_error(E_FAIL);

            co_await winrt::resume_after(std::chrono::milliseconds(25));
        }
    }
};

DeviceService::DeviceService(DeviceServiceDependencies dependencies) : m_state(std::make_shared<State>()) {
    m_state->WatcherPlatform = std::move(dependencies.WatcherPlatform);
    m_state->ConnectionPlatform = std::move(dependencies.ConnectionPlatform);
    m_state->TimerPlatform = std::move(dependencies.TimerPlatform);
    m_state->Initialize();
}

DeviceService::~DeviceService() {
    Shutdown();
}

DeviceService::Subscription DeviceService::Subscribe(FactSink factSink) {
    auto state = m_state;
    if (!state) return 0;
    auto result = std::make_shared<Subscription>(0);
    auto const ran = state->Post([state, factSink = std::move(factSink), result] mutable {
        if (state->IsShutdown) return;
        state->Subscriber = std::move(factSink);
        state->ActiveSubscription = state->NextSubscription++;
        *result = state->ActiveSubscription;
    });
    return ran ? *result : 0;
}

void DeviceService::Unsubscribe(Subscription subscription) noexcept {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state, subscription] {
        if (state->ActiveSubscription != subscription) return;
        state->Subscriber = {};
        state->ActiveSubscription = 0;
    }));
}

DeviceCommandResult DeviceService::Start() {
    auto state = m_state;
    if (!state) return {};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, result] {
        if (state->IsShutdown) {
            *result = state->Result(DeviceCommandKind::Start, DeviceCommandResultKind::Rejected);
            return;
        }
        if (state->IsRunning) {
            *result = state->Result(DeviceCommandKind::Start, DeviceCommandResultKind::Coalesced);
            return;
        }
        state->IsRunning = state->Watcher->Start();
        *result =
            state->Result(DeviceCommandKind::Start,
                          state->IsRunning ? DeviceCommandResultKind::Accepted : DeviceCommandResultKind::Rejected);
        state->Publish(DeviceFactKind::InventoryChanged);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Start, .Kind = DeviceCommandResultKind::Coalesced};
}

DeviceCommandResult DeviceService::Stop() {
    auto state = m_state;
    if (!state) return {};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, result] {
        if (state->IsShutdown) {
            *result = state->Result(DeviceCommandKind::Stop, DeviceCommandResultKind::Rejected);
            return;
        }
        if (!state->IsRunning) {
            *result = state->Result(DeviceCommandKind::Stop, DeviceCommandResultKind::Coalesced);
            return;
        }
        state->Watcher->Stop();
        state->IsRunning = false;
        *result = state->Result(DeviceCommandKind::Stop, DeviceCommandResultKind::Accepted);
        state->Publish(DeviceFactKind::InventoryChanged);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Stop, .Kind = DeviceCommandResultKind::Coalesced};
}

DeviceCommandResult DeviceService::Connect(std::wstring deviceId) {
    auto state = m_state;
    if (!state || deviceId.empty())
        return {.Command = DeviceCommandKind::Connect, .Kind = DeviceCommandResultKind::Rejected};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, deviceId = std::move(deviceId), result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Connect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        if (session->Snapshot().State == DeviceLifecycleState::Connected) {
            *result = state->Result(DeviceCommandKind::Connect, DeviceCommandResultKind::Coalesced, deviceId);
            return;
        }
        auto const wasBusy = session->IsBusy();
        session->Connect(DeviceOperationKind::ManualConnect, true);
        *result = state->Result(DeviceCommandKind::Connect,
                                wasBusy ? DeviceCommandResultKind::Coalesced : DeviceCommandResultKind::Accepted,
                                deviceId);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Connect,
                                     .Kind = DeviceCommandResultKind::Coalesced,
                                     .DeviceId = std::move(deviceId)};
}

DeviceCommandResult DeviceService::Disconnect(std::wstring deviceId) {
    auto state = m_state;
    if (!state || deviceId.empty())
        return {.Command = DeviceCommandKind::Disconnect, .Kind = DeviceCommandResultKind::Rejected};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, deviceId = std::move(deviceId), result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Disconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        session->Disconnect(state->IsIncomingEnabled);
        *result = state->Result(DeviceCommandKind::Disconnect, DeviceCommandResultKind::Accepted, deviceId);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Disconnect,
                                     .Kind = DeviceCommandResultKind::Coalesced,
                                     .DeviceId = std::move(deviceId)};
}

DeviceCommandResult DeviceService::Reconnect(std::wstring deviceId) {
    auto state = m_state;
    if (!state || deviceId.empty())
        return {.Command = DeviceCommandKind::Reconnect, .Kind = DeviceCommandResultKind::Rejected};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, deviceId = std::move(deviceId), result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        auto const wasBusy = session->IsBusy();
        session->Connect(DeviceOperationKind::ManualReconnect, true);
        *result = state->Result(DeviceCommandKind::Reconnect,
                                wasBusy ? DeviceCommandResultKind::Coalesced : DeviceCommandResultKind::Accepted,
                                deviceId);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Reconnect,
                                     .Kind = DeviceCommandResultKind::Coalesced,
                                     .DeviceId = std::move(deviceId)};
}

DeviceCommandResult DeviceService::CancelReconnect(std::wstring deviceId) {
    auto state = m_state;
    if (!state || deviceId.empty())
        return {.Command = DeviceCommandKind::Reconnect, .Kind = DeviceCommandResultKind::Rejected};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, deviceId = std::move(deviceId), result] {
        if (state->IsShutdown) {
            *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        if (auto existing = state->Sessions.find(deviceId); existing != state->Sessions.end()) {
            existing->second->CancelReconnect();
            *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Cancelled, deviceId);
            return;
        }
        *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Coalesced, deviceId);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Reconnect,
                                     .Kind = DeviceCommandResultKind::Coalesced,
                                     .DeviceId = std::move(deviceId)};
}

DeviceCommandResult DeviceService::CancelPendingReconnects() {
    auto state = m_state;
    if (!state) return {};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, result] {
        if (state->IsShutdown) {
            *result = state->Result(DeviceCommandKind::ReconnectAll, DeviceCommandResultKind::Rejected);
            return;
        }
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->CancelReconnect();
        }
        *result = state->Result(DeviceCommandKind::ReconnectAll, DeviceCommandResultKind::Cancelled);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::ReconnectAll,
                                     .Kind = DeviceCommandResultKind::Coalesced};
}

DeviceCommandResult DeviceService::DisconnectAll() {
    auto state = m_state;
    if (!state) return {};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::DisconnectAll, DeviceCommandResultKind::Rejected);
            return;
        }
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->Disconnect(state->IsIncomingEnabled);
        }
        *result = state->Result(DeviceCommandKind::DisconnectAll, DeviceCommandResultKind::Accepted);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::DisconnectAll,
                                     .Kind = DeviceCommandResultKind::Coalesced};
}

DeviceCommandResult DeviceService::ReconnectAll() {
    auto state = m_state;
    if (!state) return {};
    auto result = std::make_shared<DeviceCommandResult>();
    auto const ran = state->Post([state, result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::ReconnectAll, DeviceCommandResultKind::Rejected);
            return;
        }
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            if (session->Snapshot().State == DeviceLifecycleState::Connected) {
                session->Connect(DeviceOperationKind::ManualReconnect, true);
            }
        }
        *result = state->Result(DeviceCommandKind::ReconnectAll, DeviceCommandResultKind::Accepted);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::ReconnectAll,
                                     .Kind = DeviceCommandResultKind::Coalesced};
}

void DeviceService::ConfigureIncomingConnections(bool enabled) {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state, enabled] {
        if (state->IsShutdown) return;
        state->IsIncomingEnabled = enabled;
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->SetIncomingEnabled(enabled);
        }
        if (enabled && state->Watcher) {
            for (auto const& device : state->Watcher->Snapshot().Devices) {
                state->GetOrCreateSession(device.Id)->SetIncomingEnabled(true);
            }
        }
    }));
}

void DeviceService::ConfigureReconnectPolicy(bool globallyEnabled, std::vector<std::wstring> enabledDeviceIds) {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state, globallyEnabled, enabledDeviceIds = std::move(enabledDeviceIds)] {
        if (state->IsShutdown) return;
        state->IsGlobalReconnectEnabled = globallyEnabled;
        state->IndividuallyReconnectEnabled = {enabledDeviceIds.begin(), enabledDeviceIds.end()};
        for (auto const& [id, session] : state->Sessions) {
            session->SetReconnectEnabled(globallyEnabled || state->IndividuallyReconnectEnabled.contains(id));
        }
    }));
}

void DeviceService::ConnectStartupTargets(std::vector<std::wstring> deviceIds) {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state, deviceIds = std::move(deviceIds)] {
        if (state->IsShutdown || state->IsSuspended) return;
        for (auto const& id : deviceIds) {
            if (id.empty()) continue;
            state->GetOrCreateSession(id)->Connect(DeviceOperationKind::Startup, true);
        }
    }));
}

void DeviceService::Suspend() {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown || state->IsSuspended) return;
        state->IsSuspended = true;
        if (state->Watcher) state->Watcher->Stop();
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->Suspend();
        }
        state->Publish(DeviceFactKind::SessionChanged);
    }));
}

void DeviceService::Resume() {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown || !state->IsSuspended) return;
        state->IsSuspended = false;
        if (state->IsRunning && state->Watcher) state->IsRunning = state->Watcher->Start();
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->Resume();
        }
        state->Publish(DeviceFactKind::SessionChanged);
    }));
}

void DeviceService::Shutdown() noexcept {
    auto const state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown) return;
        state->IsShutdown = true;
        state->IsRunning = false;
        state->StopAndReleaseSessions();
        state->Publish(DeviceFactKind::Shutdown);
        state->Subscriber = {};
        state->ActiveSubscription = 0;
    }));
}

DeviceServiceSnapshot DeviceService::Snapshot() const {
    return m_state ? m_state->Snapshot() : DeviceServiceSnapshot{};
}

void DeviceService::StartDeviceWatcher() {
    static_cast<void>(Start());
}

void DeviceService::StopDeviceWatcher() {
    static_cast<void>(Stop());
}

void DeviceService::ShutdownForProcessExit() noexcept {
    Shutdown();
}

void DeviceService::SuspendForPowerTransition() noexcept {
    Suspend();
}

void DeviceService::ResumeAfterPowerTransition() {
    auto const state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown || !state->IsSuspended) return;
        state->IsSuspended = false;
        if (state->IsRunning && state->Watcher) state->IsRunning = state->Watcher->Start();
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->ResumeIdleAfterPowerTransition();
        }
        state->Publish(DeviceFactKind::SessionChanged);
    }));
}

void DeviceService::ResumeSuspendedSessions(std::vector<std::wstring> deviceIds) {
    auto const state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state, deviceIds = std::move(deviceIds)] {
        if (state->IsShutdown || state->IsSuspended) return;
        std::unordered_set<std::wstring> requested(deviceIds.begin(), deviceIds.end());
        for (auto const& [id, session] : state->Sessions) {
            auto const snapshot = session->Snapshot();
            if (requested.contains(id) || snapshot.IsIncomingEnabled || session->IsBusy()) session->Resume();
        }
        for (auto const& id : requested) {
            if (id.empty()) continue;
            auto session = state->GetOrCreateSession(id);
            if (!session) continue;
            session->Resume();
            auto const snapshot = session->Snapshot();
            if (!session->IsBusy() && snapshot.State != DeviceLifecycleState::Connected)
                session->Connect(DeviceOperationKind::Resume, true);
        }
        state->Publish(DeviceFactKind::SessionChanged);
    }));
}

void DeviceService::SetIncomingConnectionsEnabled(bool enabled) {
    ConfigureIncomingConnections(enabled);
}

void DeviceService::ApplyReconnectOnConnectionLossPolicy(bool globallyEnabled,
                                                         std::span<const std::wstring> individuallyEnabledDeviceIds) {
    ConfigureReconnectPolicy(
        globallyEnabled,
        std::vector<std::wstring>(individuallyEnabledDeviceIds.begin(), individuallyEnabledDeviceIds.end()));
}

void DeviceService::SetReconnectOnConnectionLoss(std::wstring deviceId, bool enabled) {
    auto const state = m_state;
    if (!state || deviceId.empty()) return;
    static_cast<void>(state->Post([state, deviceId = std::move(deviceId), enabled] {
        if (state->IsShutdown) return;
        if (enabled) {
            state->IndividuallyReconnectEnabled.insert(deviceId);
        } else {
            state->IndividuallyReconnectEnabled.erase(deviceId);
        }
        if (auto iter = state->Sessions.find(deviceId); iter != state->Sessions.end()) {
            iter->second->SetReconnectEnabled(enabled || state->IsGlobalReconnectEnabled);
        }
    }));
}

winrt::Windows::Foundation::IAsyncAction DeviceService::ConnectAsync(winrt::hstring deviceId) {
    auto const state = m_state;
    if (!state || deviceId.empty()) co_return;
    auto const result = Connect(std::wstring(deviceId));
    if (result.Kind == DeviceCommandResultKind::Rejected) throw winrt::hresult_error(E_ABORT);
    co_await state->AwaitTerminal(
        std::wstring(deviceId), result.OperationEpoch, result.Kind == DeviceCommandResultKind::Accepted);
}

void DeviceService::ConnectDetached(winrt::hstring deviceId) {
    static_cast<void>(Connect(std::wstring(deviceId)));
}

winrt::Windows::Foundation::IAsyncAction DeviceService::ReconnectAsync(winrt::hstring deviceId) {
    auto const state = m_state;
    if (!state || deviceId.empty()) co_return;
    auto const result = Reconnect(std::wstring(deviceId));
    if (result.Kind == DeviceCommandResultKind::Rejected) throw winrt::hresult_error(E_ABORT);
    co_await state->AwaitTerminal(
        std::wstring(deviceId), result.OperationEpoch, result.Kind == DeviceCommandResultKind::Accepted);
}

void DeviceService::ReconnectDetached(winrt::hstring deviceId) {
    static_cast<void>(Reconnect(std::wstring(deviceId)));
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
DeviceService::RefreshDevicesAsync() {
    auto const state = m_state;
    if (!state || state->Snapshot().IsShutdown || !state->Watcher) co_return nullptr;
    co_return co_await state->Watcher->RefreshAsync();
}

std::vector<DeviceSessionSnapshot> DeviceService::GetConnectedDevices() const {
    std::vector<DeviceSessionSnapshot> result;
    for (auto const& session : Snapshot().Sessions) {
        if (session.State == DeviceLifecycleState::Connected) result.push_back(session);
    }
    return result;
}

std::vector<DeviceSessionSnapshot> DeviceService::GetConnectionSessions() const {
    return Snapshot().Sessions;
}

std::vector<std::wstring> DeviceService::GetPowerTransitionRecoveryDeviceIds() const {
    std::vector<std::wstring> result;
    for (auto const& session : Snapshot().Sessions) {
        bool const requiresRecovery = session.State == DeviceLifecycleState::Connected || session.IsIncomingEnabled ||
                                      session.State == DeviceLifecycleState::Connecting ||
                                      session.State == DeviceLifecycleState::Disconnecting ||
                                      session.State == DeviceLifecycleState::WaitingForReconnect;
        if (requiresRecovery && !session.DeviceId.empty()) result.push_back(session.DeviceId);
    }
    return result;
}

bool DeviceService::IsDeviceConnected(std::wstring_view deviceId) const {
    auto const snapshot = Snapshot();
    auto const iter = std::ranges::find(snapshot.Sessions, deviceId, &DeviceSessionSnapshot::DeviceId);
    return iter != snapshot.Sessions.end() && iter->State == DeviceLifecycleState::Connected;
}

std::optional<std::wstring> DeviceService::GetConnectionDisplayName(std::wstring_view deviceId) const {
    auto const snapshot = Snapshot();
    auto const iter = std::ranges::find(snapshot.Sessions, deviceId, &DeviceSessionSnapshot::DeviceId);
    if (iter == snapshot.Sessions.end() || iter->State != DeviceLifecycleState::Connected) return std::nullopt;
    return iter->DeviceName;
}

bool DeviceService::HasConnections() const {
    return !GetConnectedDevices().empty();
}

bool DeviceService::HasBusyOperations() const {
    return std::ranges::any_of(Snapshot().Sessions, [](auto const& session) {
        return session.State == DeviceLifecycleState::Connecting ||
               session.State == DeviceLifecycleState::Disconnecting ||
               session.State == DeviceLifecycleState::WaitingForReconnect;
    });
}

bool DeviceService::IsDeviceBusy(std::wstring_view deviceId) const {
    auto const snapshot = Snapshot();
    auto const iter = std::ranges::find(snapshot.Sessions, deviceId, &DeviceSessionSnapshot::DeviceId);
    if (iter == snapshot.Sessions.end()) return false;
    return iter->State == DeviceLifecycleState::Connecting || iter->State == DeviceLifecycleState::Disconnecting ||
           iter->State == DeviceLifecycleState::WaitingForReconnect;
}

device_picker::DeviceActivitySnapshot DeviceService::GetDevicePickerActivitySnapshot() const {
    device_picker::DeviceActivitySnapshot result;
    for (auto const& session : Snapshot().Sessions) {
        if (session.State == DeviceLifecycleState::Connected) result.ConnectedIds.insert(session.DeviceId);
        if (session.State == DeviceLifecycleState::Connecting || session.State == DeviceLifecycleState::Disconnecting ||
            session.State == DeviceLifecycleState::WaitingForReconnect) {
            result.BusyIds.insert(session.DeviceId);
        }
    }
    return result;
}

device_picker::DeviceInventorySnapshot DeviceService::GetDevicePickerInventorySnapshot() const {
    return Snapshot().Inventory;
}

std::optional<device_picker::DeviceInventorySnapshot>
DeviceService::GetDevicePickerInventorySnapshotIfChanged(std::uint64_t knownGeneration) const {
    auto const inventory = Snapshot().Inventory;
    if (inventory.Generation == knownGeneration) return std::nullopt;
    return inventory;
}

DeviceTrayPresentationSnapshot DeviceService::GetTrayPresentationSnapshot() const {
    DeviceTrayPresentationSnapshot result;
    auto const snapshot = Snapshot();
    for (auto const& session : snapshot.Sessions) {
        result.HasBusyOperations = result.HasBusyOperations || session.State == DeviceLifecycleState::Connecting ||
                                   session.State == DeviceLifecycleState::Disconnecting ||
                                   session.State == DeviceLifecycleState::WaitingForReconnect;
        if (session.State == DeviceLifecycleState::Connected)
            result.ConnectedDevices.push_back({.Id = session.DeviceId, .Name = session.DeviceName});
    }
    return result;
}

} // namespace apc::device
