#include <pch.h>

#include <core/DeviceService.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace apc::device {

struct DeviceService::State : std::enable_shared_from_this<DeviceService::State> {
    using Task = std::function<void()>;

    std::mutex QueueMutex;
    std::deque<Task> Queue;
    bool IsExecuting = false;

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
                if (auto service = weak.lock()) service->Post(std::move(task));
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
        {
            std::lock_guard guard(QueueMutex);
            Queue.push_back(std::move(task));
            if (!IsExecuting) {
                IsExecuting = true;
                runsHere = true;
            }
        }
        if (!runsHere) return false;

        for (;;) {
            Task next;
            {
                std::lock_guard guard(QueueMutex);
                if (Queue.empty()) {
                    IsExecuting = false;
                    break;
                }
                next = std::move(Queue.front());
                Queue.pop_front();
            }
            next();
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
                if (auto service = weak.lock()) service->Post(std::move(task));
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
        Publish(fact.IsTerminalFailure || fact.Result == DeviceConnectionResult::Failed
                    ? DeviceFactKind::OperationFailed
                    : DeviceFactKind::SessionChanged,
                fact.Snapshot.DeviceId,
                fact.Result,
                fact.IsTerminalFailure);
    }

    void OnWatcherFact(DeviceWatcherFact const& fact) {
        if (IsShutdown) return;
        if (fact.Kind == DeviceWatcherFactKind::DeviceAdded) {
            auto session = GetOrCreateSession(fact.DeviceId);
            if (session) session->Rename(fact.DeviceName);
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
    state->Post([state, subscription] {
        if (state->ActiveSubscription != subscription) return;
        state->Subscriber = {};
        state->ActiveSubscription = 0;
    });
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
            *result = state->Result(DeviceCommandKind::Start, DeviceCommandResultKind::Rejected);
            return;
        }
        if (!state->IsRunning) {
            *result = state->Result(DeviceCommandKind::Start, DeviceCommandResultKind::Coalesced);
            return;
        }
        state->Watcher->Stop();
        state->IsRunning = false;
        *result = state->Result(DeviceCommandKind::Start, DeviceCommandResultKind::Accepted);
        state->Publish(DeviceFactKind::InventoryChanged);
    });
    return ran ? *result
               : DeviceCommandResult{.Command = DeviceCommandKind::Start, .Kind = DeviceCommandResultKind::Coalesced};
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
    state->Post([state, enabled] {
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
    });
}

void DeviceService::ConfigureReconnectPolicy(bool globallyEnabled, std::vector<std::wstring> enabledDeviceIds) {
    auto state = m_state;
    if (!state) return;
    state->Post([state, globallyEnabled, enabledDeviceIds = std::move(enabledDeviceIds)] {
        if (state->IsShutdown) return;
        state->IsGlobalReconnectEnabled = globallyEnabled;
        state->IndividuallyReconnectEnabled = {enabledDeviceIds.begin(), enabledDeviceIds.end()};
        for (auto const& [id, session] : state->Sessions) {
            session->SetReconnectEnabled(globallyEnabled || state->IndividuallyReconnectEnabled.contains(id));
        }
    });
}

void DeviceService::ConnectStartupTargets(std::vector<std::wstring> deviceIds) {
    auto state = m_state;
    if (!state) return;
    state->Post([state, deviceIds = std::move(deviceIds)] {
        if (state->IsShutdown || state->IsSuspended) return;
        for (auto const& id : deviceIds) {
            if (id.empty()) continue;
            state->GetOrCreateSession(id)->Connect(DeviceOperationKind::Startup, true);
        }
    });
}

void DeviceService::Suspend() {
    auto state = m_state;
    if (!state) return;
    state->Post([state] {
        if (state->IsShutdown || state->IsSuspended) return;
        state->IsSuspended = true;
        if (state->Watcher) state->Watcher->Stop();
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->Suspend();
        }
        state->Publish(DeviceFactKind::SessionChanged);
    });
}

void DeviceService::Resume() {
    auto state = m_state;
    if (!state) return;
    state->Post([state] {
        if (state->IsShutdown || !state->IsSuspended) return;
        state->IsSuspended = false;
        if (state->IsRunning && state->Watcher) state->Watcher->Start();
        for (auto const& [id, session] : state->Sessions) {
            (void)id;
            session->Resume();
        }
        state->Publish(DeviceFactKind::SessionChanged);
    });
}

void DeviceService::Shutdown() noexcept {
    auto state = std::exchange(m_state, {});
    if (!state) return;
    state->Post([state] {
        if (state->IsShutdown) return;
        state->IsShutdown = true;
        state->IsRunning = false;
        state->StopAndReleaseSessions();
        state->Publish(DeviceFactKind::Shutdown);
        state->Subscriber = {};
        state->ActiveSubscription = 0;
    });
}

DeviceServiceSnapshot DeviceService::Snapshot() const {
    return m_state ? m_state->Snapshot() : DeviceServiceSnapshot{};
}

} // namespace apc::device
