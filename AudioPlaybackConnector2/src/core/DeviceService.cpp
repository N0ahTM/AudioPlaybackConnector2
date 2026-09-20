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

// Only the serialized device owner resolves an operation. Its first terminal result is immutable, even when the
// session subsequently disconnects or starts a new epoch. Waiters never infer completion from a later snapshot.
struct DeviceOperationCompletion {
    std::weak_ptr<void> Owner;
    std::wstring DeviceId;
    std::uint64_t Epoch = 0;
    bool OwnsCancellation = false;
    std::mutex Mutex;
    std::condition_variable_any Changed;
    std::optional<DeviceOperationStatus> Status;

    void Resolve(DeviceOperationStatus status) {
        {
            std::lock_guard guard(Mutex);
            if (Status) return;
            Status = status;
        }
        Changed.notify_all();
    }
};

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

    util::LogSink Log;
    std::unique_ptr<DeviceWatcherPlatform> WatcherPlatform;
    std::unique_ptr<DeviceConnectionPlatform> ConnectionPlatform;
    std::unique_ptr<DeviceTimerPlatform> TimerPlatform;
    std::unique_ptr<DeviceWatcher> Watcher;
    std::unordered_map<std::wstring, std::shared_ptr<DeviceSession>> Sessions;
    std::vector<std::weak_ptr<DeviceOperationCompletion>> PendingOperations;
    std::unordered_set<std::wstring> IndividuallyReconnectEnabled;
    std::unordered_map<std::wstring, std::uint64_t> PowerTransitionRecoveryEpochs;
    FactSink Subscriber;
    Subscription ActiveSubscription = 0;
    Subscription NextSubscription = 1;
    std::uint64_t Generation = 0;
    bool IsRunning = false;
    bool IsSuspended = false;
    bool IsShutdown = false;
    bool IsIncomingEnabled = false;
    bool IsGlobalReconnectEnabled = true;
    std::optional<std::uint64_t> SettingsPolicyRevision;

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
            std::move(WatcherPlatform),
            Log);
        UpdatePublishedSnapshot();
    }

    // The queue lock protects queue bookkeeping only. Tasks, platform calls, and fact publication run after it is
    // released.
    [[nodiscard]] bool Post(Task task, bool waitForCompletion = true) {
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
            } else if (waitForCompletion && ExecutorThread != std::this_thread::get_id()) {
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
                Log.UnknownException(L"[DeviceService] serialized task failed");
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
        {
            std::lock_guard guard(SnapshotMutex);
            PublishedSnapshot = std::move(snapshot);
        }
        std::erase_if(PendingOperations, [this](auto const& weak) {
            auto completion = weak.lock();
            return !completion || ResolveOperation(*completion);
        });
    }

    [[nodiscard]] bool ResolveOperation(DeviceOperationCompletion& completion) const {
        auto const session = Sessions.find(completion.DeviceId);
        if (IsShutdown || session == Sessions.end()) {
            completion.Resolve(DeviceOperationStatus::Cancelled);
            return true;
        }
        auto const snapshot = session->second->Snapshot();
        if (snapshot.OperationEpoch != completion.Epoch || snapshot.State == DeviceLifecycleState::Idle) {
            completion.Resolve(DeviceOperationStatus::Cancelled);
            return true;
        }
        if (snapshot.State == DeviceLifecycleState::Connected) {
            completion.Resolve(DeviceOperationStatus::Succeeded);
            return true;
        }
        if (snapshot.State == DeviceLifecycleState::Failed) {
            completion.Resolve(DeviceOperationStatus::Failed);
            return true;
        }
        return false;
    }

    void Publish(DeviceFactKind kind,
                 std::wstring deviceId = {},
                 DeviceConnectionResult result = DeviceConnectionResult::Success,
                 bool terminal = false,
                 DeviceOperationKind operation = DeviceOperationKind::ManualConnect,
                 DeviceDisconnectReason disconnectReason = DeviceDisconnectReason::None) {
        ++Generation;
        UpdatePublishedSnapshot();
        if (Subscriber)
            Subscriber({.Kind = kind,
                        .Snapshot = Snapshot(),
                        .DeviceId = std::move(deviceId),
                        .ConnectionResult = result,
                        .Operation = operation,
                        .IsTerminalFailure = terminal,
                        .DisconnectReason = disconnectReason});
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
        auto const isExceptionalDisconnect = fact.DisconnectReason == DeviceDisconnectReason::UnexpectedLoss ||
                                             fact.DisconnectReason == DeviceDisconnectReason::DeviceRemoved;
        Publish(isExceptionalDisconnect || !isFailure ? DeviceFactKind::SessionChanged
                                                      : DeviceFactKind::OperationFailed,
                fact.Snapshot.DeviceId,
                fact.Result,
                fact.IsTerminalFailure,
                fact.Operation,
                fact.DisconnectReason);
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
    Result(DeviceCommandKind command, DeviceCommandResultKind kind, std::wstring deviceId = {}) {
        DeviceCommandResult result;
        result.Command = command;
        result.Kind = kind;
        result.DeviceId = std::move(deviceId);
        if (!result.DeviceId.empty()) {
            if (auto existing = Sessions.find(result.DeviceId); existing != Sessions.end()) {
                result.OperationEpoch = existing->second->Snapshot().OperationEpoch;
            }
        }
        if ((command == DeviceCommandKind::Connect || command == DeviceCommandKind::Reconnect) &&
            (kind == DeviceCommandResultKind::Accepted || kind == DeviceCommandResultKind::Coalesced)) {
            auto completion = std::make_shared<DeviceOperationCompletion>();
            completion->Owner = shared_from_this();
            completion->DeviceId = result.DeviceId;
            completion->Epoch = result.OperationEpoch;
            completion->OwnsCancellation = kind == DeviceCommandResultKind::Accepted;
            std::erase_if(PendingOperations, [](auto const& weak) { return weak.expired(); });
            if (!ResolveOperation(*completion)) PendingOperations.push_back(completion);
            result.Completion = std::move(completion);
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
        iter->second->Disconnect(IsIncomingEnabled);
    }

    [[nodiscard]] static bool RequiresPowerTransitionRecovery(DeviceSessionSnapshot const& snapshot) noexcept {
        return snapshot.State == DeviceLifecycleState::Connected || snapshot.IsIncomingEnabled ||
               snapshot.State == DeviceLifecycleState::Connecting ||
               snapshot.State == DeviceLifecycleState::Disconnecting ||
               snapshot.State == DeviceLifecycleState::WaitingForReconnect;
    }

    [[nodiscard]] std::vector<std::wstring> CapturePowerTransitionRecoveryAndSuspend() {
        std::vector<std::wstring> recoveryDeviceIds;
        if (IsShutdown || IsSuspended) return recoveryDeviceIds;

        for (auto const& [deviceId, session] : Sessions) {
            if (RequiresPowerTransitionRecovery(session->Snapshot())) recoveryDeviceIds.push_back(deviceId);
        }

        IsSuspended = true;
        if (Watcher) Watcher->Stop();
        for (auto const& [deviceId, session] : Sessions) {
            (void)deviceId;
            session->Suspend();
        }

        PowerTransitionRecoveryEpochs.clear();
        for (auto const& deviceId : recoveryDeviceIds) {
            if (auto session = Sessions.find(deviceId); session != Sessions.end()) {
                PowerTransitionRecoveryEpochs.emplace(deviceId, session->second->Snapshot().OperationEpoch);
            }
        }
        Publish(DeviceFactKind::SessionChanged);
        return recoveryDeviceIds;
    }
};

DeviceService::DeviceService(DeviceServiceDependencies dependencies) : m_state(std::make_shared<State>()) {
    m_state->Log = std::move(dependencies.Log);
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
    auto const ran = state->Post([state, deviceId, result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Connect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        auto const supersedesPowerTransitionRecovery = session->IsSuspended();
        state->PowerTransitionRecoveryEpochs.erase(deviceId);
        auto const snapshot = session->Snapshot();
        if (session->IsBusy() && snapshot.State == DeviceLifecycleState::Disconnecting &&
            !supersedesPowerTransitionRecovery) {
            *result = state->Result(DeviceCommandKind::Connect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        if (snapshot.State == DeviceLifecycleState::Connected) {
            *result = state->Result(DeviceCommandKind::Connect, DeviceCommandResultKind::Coalesced, deviceId);
            return;
        }
        auto const operationEpoch = session->Snapshot().OperationEpoch;
        session->Connect(DeviceOperationKind::ManualConnect, true);
        *result =
            state->Result(DeviceCommandKind::Connect,
                          session->Snapshot().OperationEpoch == operationEpoch ? DeviceCommandResultKind::Coalesced
                                                                               : DeviceCommandResultKind::Accepted,
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
    auto const ran = state->Post([state, deviceId, result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Disconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        if (!session->Disconnect(state->IsIncomingEnabled)) {
            *result = state->Result(DeviceCommandKind::Disconnect, DeviceCommandResultKind::Coalesced, deviceId);
            return;
        }
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
    auto const ran = state->Post([state, deviceId, result] {
        if (state->IsShutdown || state->IsSuspended) {
            *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto session = state->GetOrCreateSession(deviceId);
        auto const supersedesPowerTransitionRecovery = session->IsSuspended();
        state->PowerTransitionRecoveryEpochs.erase(deviceId);
        auto const snapshot = session->Snapshot();
        if (session->IsBusy() && snapshot.State == DeviceLifecycleState::Disconnecting &&
            !supersedesPowerTransitionRecovery) {
            *result = state->Result(DeviceCommandKind::Reconnect, DeviceCommandResultKind::Rejected, deviceId);
            return;
        }
        auto const operationEpoch = snapshot.OperationEpoch;
        session->Connect(DeviceOperationKind::ManualReconnect, true);
        *result =
            state->Result(DeviceCommandKind::Reconnect,
                          session->Snapshot().OperationEpoch == operationEpoch ? DeviceCommandResultKind::Coalesced
                                                                               : DeviceCommandResultKind::Accepted,
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
    auto const ran = state->Post([state, deviceId, result] {
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

void DeviceService::ApplySettingsPolicy(DeviceSettingsPolicy policy) {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post(
        [state, policy = std::move(policy)] {
            if (state->IsShutdown || policy.StopToken.stop_requested() ||
                (state->SettingsPolicyRevision && policy.Revision <= *state->SettingsPolicyRevision))
                return;
            std::unordered_set<std::wstring> reconnectIds(policy.ReconnectDeviceIds.begin(),
                                                          policy.ReconnectDeviceIds.end());
            const bool reconnectChanged = state->IsGlobalReconnectEnabled != policy.GlobalReconnectOnConnectionLoss ||
                                          state->IndividuallyReconnectEnabled != reconnectIds;
            const bool incomingChanged = state->IsIncomingEnabled != policy.AllowIncomingConnections;
            state->SettingsPolicyRevision = policy.Revision;
            state->IsGlobalReconnectEnabled = policy.GlobalReconnectOnConnectionLoss;
            state->IndividuallyReconnectEnabled.swap(reconnectIds);
            state->IsIncomingEnabled = policy.AllowIncomingConnections;
            for (auto const& [id, session] : state->Sessions) {
                if (reconnectChanged)
                    session->SetReconnectEnabled(state->IsGlobalReconnectEnabled ||
                                                 state->IndividuallyReconnectEnabled.contains(id));
                if (incomingChanged) session->SetIncomingEnabled(state->IsIncomingEnabled);
            }
            if (incomingChanged && state->IsIncomingEnabled && state->Watcher) {
                auto const inventory = state->Watcher->Snapshot();
                for (auto const& device : inventory.Devices)
                    (void)state->GetOrCreateSession(device.Id);
            }
        },
        false));
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
    static_cast<void>(state->Post([state] { static_cast<void>(state->CapturePowerTransitionRecoveryAndSuspend()); }));
}

void DeviceService::Resume() {
    auto state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown || !state->IsSuspended) return;
        state->IsSuspended = false;
        state->PowerTransitionRecoveryEpochs.clear();
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

std::vector<std::wstring> DeviceService::SuspendForPowerTransition() {
    auto state = m_state;
    if (!state) return {};
    auto recoveryDeviceIds = std::make_shared<std::vector<std::wstring>>();
    auto const ran = state->Post(
        [state, recoveryDeviceIds] { *recoveryDeviceIds = state->CapturePowerTransitionRecoveryAndSuspend(); });
    return ran ? std::move(*recoveryDeviceIds) : std::vector<std::wstring>{};
}

void DeviceService::ResumeAfterPowerTransition() {
    auto const state = m_state;
    if (!state) return;
    static_cast<void>(state->Post([state] {
        if (state->IsShutdown) return;
        auto const wasSuspended = state->IsSuspended;
        state->IsSuspended = false;
        if (state->IsRunning && state->Watcher) {
            if (!wasSuspended) state->Watcher->Stop();
            state->IsRunning = state->Watcher->Start();
        }
        if (wasSuspended) {
            for (auto const& [id, session] : state->Sessions) {
                (void)id;
                session->ResumeIdleAfterPowerTransition();
            }
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
        for (auto const& id : requested) {
            if (id.empty()) continue;
            auto const session = state->Sessions.find(id);
            auto const recoveryEpoch = state->PowerTransitionRecoveryEpochs.find(id);
            if (session == state->Sessions.end() || recoveryEpoch == state->PowerTransitionRecoveryEpochs.end() ||
                session->second->Snapshot().OperationEpoch != recoveryEpoch->second) {
                if (recoveryEpoch != state->PowerTransitionRecoveryEpochs.end()) {
                    if (session != state->Sessions.end()) session->second->CancelPowerTransitionRecovery();
                    state->PowerTransitionRecoveryEpochs.erase(recoveryEpoch);
                }
                continue;
            }
            session->second->Resume();
            state->PowerTransitionRecoveryEpochs.erase(recoveryEpoch);
        }
        state->Publish(DeviceFactKind::SessionChanged);
    }));
}

DeviceOperationStatus DeviceService::WaitForCompletion(DeviceCommandResult const& command,
                                                       std::stop_token stopToken,
                                                       std::chrono::steady_clock::time_point deadline) {
    auto const state = m_state;
    auto const completion = command.Completion;
    if (!state || !completion || completion->Owner.lock() != state) return DeviceOperationStatus::Rejected;
    {
        std::lock_guard guard(state->QueueMutex);
        // A fact handler must not block the context that must deliver this operation's terminal transition.
        if (state->IsExecuting && state->ExecutorThread == std::this_thread::get_id())
            return DeviceOperationStatus::Rejected;
    }
    DeviceOperationStatus outcome;
    {
        std::unique_lock lock(completion->Mutex);
        if (completion->Changed.wait_until(lock, stopToken, deadline, [&] { return completion->Status.has_value(); }))
            return *completion->Status;
        outcome = stopToken.stop_requested() ? DeviceOperationStatus::Cancelled : DeviceOperationStatus::TimedOut;
    }
    if (completion->OwnsCancellation) {
        // Queue cancellation without waiting behind another publisher. The epoch and terminal check protect a
        // replacement operation and an operation which completed while this cancellation was queued.
        (void)state->Post(
            [state, completion] {
                {
                    std::lock_guard guard(completion->Mutex);
                    if (completion->Status) return;
                }
                state->CancelOperation(completion->DeviceId, completion->Epoch);
            },
            false);
    }
    return outcome;
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
DeviceService::RefreshDevicesAsync() {
    auto const state = m_state;
    if (!state || state->Snapshot().IsShutdown || !state->Watcher) co_return nullptr;
    co_return co_await state->Watcher->RefreshAsync();
}

std::vector<DeviceSessionSnapshot> DeviceService::GetConnectedDevices() const {
    std::vector<DeviceSessionSnapshot> result;
    auto const snapshot = Snapshot();
    for (auto const& session : snapshot.Sessions) {
        if (session.State == DeviceLifecycleState::Connected) result.push_back(session);
    }
    return result;
}

std::vector<std::wstring> DeviceService::GetPowerTransitionRecoveryDeviceIds() const {
    std::vector<std::wstring> result;
    auto const snapshot = Snapshot();
    for (auto const& session : snapshot.Sessions) {
        if (State::RequiresPowerTransitionRecovery(session) && !session.DeviceId.empty())
            result.push_back(session.DeviceId);
    }
    return result;
}

bool DeviceService::IsDeviceConnected(std::wstring_view deviceId) const {
    auto const snapshot = Snapshot();
    auto const iter = std::ranges::find(snapshot.Sessions, deviceId, &DeviceSessionSnapshot::DeviceId);
    return iter != snapshot.Sessions.end() && iter->State == DeviceLifecycleState::Connected;
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

} // namespace apc::device
