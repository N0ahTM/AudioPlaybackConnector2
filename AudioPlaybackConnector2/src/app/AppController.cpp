#include <app/AppController.hpp>
#include <app/StartupTaskCoordinator.hpp>
#include <core/DeviceService.hpp>
#include <core/SettingsLimits.hpp>
#include <wil/resource.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace apc::app {

namespace {
apc::device::DeviceSettingsPolicy DevicePolicy(SettingsSnapshot const& snapshot) {
    apc::device::DeviceSettingsPolicy policy{
        snapshot.Revision, snapshot.Data.AllowIncomingConnections, snapshot.Data.GlobalReconnectOnConnectionLoss, {}};
    for (auto const& device : snapshot.Data.Devices) {
        if (device.ReconnectOnConnectionLoss) policy.ReconnectDeviceIds.push_back(device.Id);
    }
    return policy;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Ordered Event Delivery ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct AppController::EventState {
    explicit EventState(std::shared_ptr<SettingsStore> settings) : Settings(std::move(settings)) {}

    struct Entry {
        Entry(SubscriptionId id, EventHandler handler) : Id(id), Handler(std::move(handler)) {}

        SubscriptionId Id;
        EventHandler Handler;
        bool Active = true;
        bool InFlight = false;
    };

    struct Delivery {
        EventNotification Notification;
        std::vector<std::shared_ptr<Entry>> Recipients;
    };

    void Remove(SubscriptionId id) noexcept {
        std::shared_ptr<Entry> removed;
        {
            std::unique_lock lock(Mutex);
            auto const found = std::ranges::find_if(Handlers, [id](auto const& entry) { return entry->Id == id; });
            if (found == Handlers.end()) return;
            removed = std::move(*found);
            Handlers.erase(found);
            removed->Active = false;
            if (DrainThread != std::this_thread::get_id()) Changed.wait(lock, [&] { return !removed->InFlight; });
        }
        // Handler captures may own another subscription or reenter application code when released.
    }

    void RequestStop() noexcept {
        SettingsCancellation.request_stop();
        std::vector<std::shared_ptr<Entry>> removed;
        std::deque<Delivery> discarded;
        {
            std::lock_guard lock(Mutex);
            Closed = true;
            for (auto const& entry : Handlers)
                entry->Active = false;
            removed.swap(Handlers);
            discarded.swap(Pending);
        }
    }

    void Drain() noexcept {
        std::unique_lock lock(Mutex);
        Changed.wait(lock, [this] {
            return ActiveDeviceObservations == 0 && (!Draining || DrainThread == std::this_thread::get_id());
        });
    }

    void Publish(AppEvent const& event, std::optional<DeviceFactPublicationFence::Token> token = {}) {
        {
            std::lock_guard lock(Mutex);
            if (Closed || NextRevision == 0) return;
            if (auto startup = std::get_if<StartupTaskChangedEvent>(&event)) {
                if (LastStartupPublication && startup->Snapshot.Publication <= *LastStartupPublication) return;
                LastStartupPublication = startup->Snapshot.Publication;
            }
            Pending.push_back({{NextRevision, event, std::move(token)}, Handlers});
            ++NextRevision;
            if (Draining) return;
            Draining = true;
            DrainThread = std::this_thread::get_id();
        }
        for (;;) {
            std::optional<Delivery> delivery;
            {
                std::lock_guard lock(Mutex);
                if (Pending.empty()) {
                    Draining = false;
                    DrainThread = {};
                    Changed.notify_all();
                    return;
                }
                delivery.emplace(std::move(Pending.front()));
                Pending.pop_front();
            }
            for (auto const& entry : delivery->Recipients) {
                {
                    std::lock_guard lock(Mutex);
                    if (!entry->Active || Closed) continue;
                    entry->InFlight = true;
                }
                try {
                    entry->Handler(delivery->Notification);
                } catch (...) {
                    // An observer failure must not interrupt ordered delivery to the remaining observers.
                }
                {
                    std::lock_guard lock(Mutex);
                    entry->InFlight = false;
                }
                Changed.notify_all();
            }
        }
    }

    void ObserveDevice(apc::device::DeviceFact const& fact) {
        using namespace apc::device;
        using Status = DeviceFactPublicationFence::Status;
        {
            std::lock_guard lock(Mutex);
            if (Closed) return;
            ++ActiveDeviceObservations;
        }
        auto finished = wil::scope_exit([this] {
            std::lock_guard lock(Mutex);
            --ActiveDeviceObservations;
            Changed.notify_all();
        });
        if (fact.Kind == DeviceFactKind::Shutdown) return;
        if (fact.Kind == DeviceFactKind::InventoryChanged) {
            Publish(DeviceInventoryChangedEvent{});
            return;
        }
        auto const session = std::ranges::find(fact.Snapshot.Sessions, fact.DeviceId, &DeviceSessionSnapshot::DeviceId);
        auto const id = ExternalDeviceId::TryCreate(fact.DeviceId);
        if (session == fact.Snapshot.Sessions.end() || !id) {
            Publish(DeviceActivityChangedEvent{});
            return;
        }
        auto const [status, appState] = [&] {
            switch (session->State) {
                case DeviceLifecycleState::Idle: return std::pair{Status::Ready, DeviceConnectionState::Idle};
                case DeviceLifecycleState::Connecting:
                    return std::pair{Status::Connecting, DeviceConnectionState::Connecting};
                case DeviceLifecycleState::Disconnecting:
                    return std::pair{Status::Connecting, DeviceConnectionState::Disconnecting};
                case DeviceLifecycleState::Connected:
                    return std::pair{Status::Connected, DeviceConnectionState::Connected};
                case DeviceLifecycleState::WaitingForReconnect:
                    return std::pair{Status::WaitingForReconnect, DeviceConnectionState::WaitingForReconnect};
                case DeviceLifecycleState::Failed: return std::pair{Status::Error, DeviceConnectionState::Failed};
            }
            return std::pair{Status::None, DeviceConnectionState::Idle};
        }();
        auto const observed = DeviceFence.Observe(fact.DeviceId, status);
        if (!observed.WasConnected && status == Status::Connected) {
            // The native owner serializes connection facts. Persist each real
            // connected transition before publishing it to UI/transport observers.
            auto const name = apc::limits::TruncateUtf16(session->DeviceName, apc::limits::c_maxDeviceNameCharacters);
            (void)Settings->RecordConnectedDevice(fact.DeviceId, name);
            Publish(DeviceConnectedEvent{*id}, observed.Connection);
        } else if (observed.WasConnected && status != Status::Connected &&
                   fact.DisconnectReason != DeviceDisconnectReason::None) {
            const bool notify = fact.DisconnectReason == DeviceDisconnectReason::UnexpectedLoss ||
                                fact.DisconnectReason == DeviceDisconnectReason::DeviceRemoved;
            Publish(DeviceDisconnectedEvent{*id, notify}, observed.Connection);
        }
        Publish(DeviceStatusChangedEvent{*id, appState}, observed.State);
        if (status == Status::WaitingForReconnect && !observed.WasWaitingForReconnect)
            Publish(AutoReconnectTriggeredEvent{*id}, observed.State);
        if (fact.Kind == DeviceFactKind::OperationFailed) {
            const bool automaticFailure =
                fact.IsTerminalFailure && fact.Operation == DeviceOperationKind::AutomaticReconnect;
            if (session->State == DeviceLifecycleState::Failed) {
                using Reason = DeviceConnectionErrorEvent::Reason;
                auto reason = automaticFailure ? Reason::ReconnectExhausted : Reason::Unknown;
                if (fact.ConnectionResult == DeviceConnectionResult::TimedOut) reason = Reason::TimedOut;
                if (fact.ConnectionResult == DeviceConnectionResult::Denied) reason = Reason::Denied;
                Publish(DeviceConnectionErrorEvent{*id, AppResultCode::OperationFailed, reason}, observed.State);
            }
            if (automaticFailure) Publish(AutoReconnectFailedEvent{*id}, observed.State);
        }
        Publish(DeviceActivityChangedEvent{});
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    // Admitted device callbacks retain this state until shutdown has drained
    // their settings commits. Store subscriptions capture only a weak EventState.
    std::shared_ptr<SettingsStore> Settings;
    std::size_t ActiveDeviceObservations = 0;
    DeviceFactPublicationFence DeviceFence;
    std::stop_source SettingsCancellation;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<std::shared_ptr<Entry>> Handlers;
    std::deque<Delivery> Pending;
    std::optional<std::uint64_t> LastStartupPublication;
    SubscriptionId NextId = 1;
    std::uint64_t NextRevision = 1;
    std::thread::id DrainThread;
    bool Draining = false;
    bool Closed = false;
};

AppController::AppController(std::shared_ptr<SettingsStore> settings,
                             std::shared_ptr<apc::device::DeviceService> devices,
                             std::weak_ptr<AppPresentation> presentation,
                             std::shared_ptr<StartupTaskCoordinator> startupTask)
    : m_settings(std::move(settings)), m_presentation(std::move(presentation)),
      m_eventState(std::make_shared<EventState>(m_settings)), m_devices(std::move(devices)),
      m_startupTask(std::move(startupTask)) {
    if (!m_settings || !m_devices) throw std::invalid_argument("application owners are required");
    std::weak_ptr<EventState> weak = m_eventState;
    m_deviceSubscription = m_devices->Subscribe([weak](apc::device::DeviceFact const& fact) {
        if (auto state = weak.lock()) state->ObserveDevice(fact);
    });
    std::weak_ptr<apc::device::DeviceService> weakDevices = m_devices;
    m_settingsSubscription = m_settings->Subscribe([weak, weakDevices](SettingsSnapshot const& snapshot) {
        auto state = weak.lock();
        if (!state || state->SettingsCancellation.stop_requested()) return;
        auto policy = DevicePolicy(snapshot);
        policy.StopToken = state->SettingsCancellation.get_token();
        if (auto devices = weakDevices.lock()) devices->ApplySettingsPolicy(std::move(policy));
        state->Publish(
            SettingsChangedEvent{snapshot.Revision, snapshot.Data.Language, snapshot.Data.UseSystemBackdropEffects});
    });
    auto policy = DevicePolicy(m_settings->Snapshot());
    policy.StopToken = m_eventState->SettingsCancellation.get_token();
    m_devices->ApplySettingsPolicy(std::move(policy));
    if (m_startupTask) {
        m_startupSubscription = m_startupTask->Subscribe([weak](StartupTaskSnapshot const& snapshot) {
            if (auto state = weak.lock()) state->Publish(StartupTaskChangedEvent{snapshot});
        });
    }
}

AppController::StartupConnectionStatus AppController::RestoreStartupConnections() const {
    CallLease lease(*this);
    if (!lease.Acquired()) return StartupConnectionStatus::Unavailable;
    auto const settings = m_settings->Snapshot().Data;
    std::vector<std::wstring> targets;
    auto append = [&](std::wstring const& id) {
        auto const device = std::ranges::find(settings.Devices, id, &DeviceSettings::Id);
        if (device != settings.Devices.end() && (settings.GlobalConnectOnStartup || device->ConnectOnStartup) &&
            !std::ranges::contains(targets, id))
            targets.push_back(id);
    };
    // SettingsStore guarantees valid, bounded identities. Recent connections take
    // priority; eligible saved devices without history follow in saved order.
    for (auto const& id : settings.LastConnectedIds)
        append(id);
    for (auto const& device : settings.Devices)
        append(device.Id);
    if (targets.empty()) return StartupConnectionStatus::NoTargets;
    m_devices->ConnectStartupTargets(std::move(targets));
    return StartupConnectionStatus::Submitted;
}

AppController::~AppController() {
    Shutdown();
    m_settingsSubscription.Reset();
    if (m_startupTask) m_startupTask->Unsubscribe(m_startupSubscription);
    if (m_devices && m_deviceSubscription) m_devices->Unsubscribe(m_deviceSubscription);
}

void AppController::RequestStop() noexcept {
    {
        std::lock_guard lock(m_stateMutex);
        if (m_running) {
            m_running = false;
            AdvanceGeneration(m_generation);
        }
    }
    m_eventState->RequestStop();
}

void AppController::Shutdown() noexcept {
    RequestStop();
    {
        std::unique_lock lock(m_stateMutex);
        m_noActiveCalls.wait(lock, [this] { return m_activeCalls == 0; });
    }
    // Join only from the lifecycle owner, never from a call being drained.
    m_eventState->Drain();
}

AppController::Subscription AppController::Subscribe(EventHandler handler) {
    if (!handler) return {};

    auto const state = m_eventState;
    if (!state) return {};

    auto entry = std::make_shared<EventState::Entry>(0, std::move(handler));
    std::scoped_lock lock(state->Mutex);
    if (state->Closed || state->NextId == 0) return {};
    auto const id = state->NextId++;
    entry->Id = id;
    state->Handlers.push_back(entry);
    return Subscription(state, id);
}

AppController::Observation AppController::SnapshotAndSubscribe(EventHandler handler) {
    Observation unavailable;
    unavailable.Snapshot.IsRunning = false;
    if (!handler) return unavailable;
    CallLease lease(*this);
    if (!lease.Acquired()) return unavailable;

    auto const state = m_eventState;
    auto entry = std::make_shared<EventState::Entry>(0, std::move(handler));
    for (std::size_t attempt = 0; attempt != 3; ++attempt) {
        std::uint64_t revision;
        {
            std::scoped_lock lock(state->Mutex);
            if (state->Closed || state->NextId == 0 || state->NextRevision == 0) return unavailable;
            revision = state->NextRevision - 1;
        }
        auto snapshot = Snapshot();
        if (!snapshot.IsRunning) return unavailable;
        {
            std::scoped_lock lock(state->Mutex);
            if (state->Closed || state->NextId == 0) return unavailable;
            // No owner or presentation calls under the publication mutex.
            // Updates preceding this point either force a fresh capture or
            // appear in the initial snapshot. Later updates target this entry.
            if (state->NextRevision - 1 != revision) continue;
            auto const id = state->NextId++;
            entry->Id = id;
            state->Handlers.push_back(entry);
            return {std::move(snapshot), revision, Subscription(state, id)};
        }
    }
    return unavailable;
}

void AppController::Publish(AppEvent const& event) const noexcept {
    auto const state = m_eventState;
    if (!state) return;

    try {
        state->Publish(event);
    } catch (...) {
        // Allocation failure cannot escape a device fact callback.
    }
}

bool AppController::IsCurrent(EventNotification const& notification) const {
    return !notification.DeviceToken || m_eventState->DeviceFence.IsCurrent(*notification.DeviceToken);
}

AppController::Subscription::~Subscription() {
    Reset();
}

AppController::Subscription::Subscription(Subscription&& other) noexcept
    : m_state(std::move(other.m_state)), m_id(std::exchange(other.m_id, 0)) {}

AppController::Subscription& AppController::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_state = std::move(other.m_state);
    m_id = std::exchange(other.m_id, 0);
    return *this;
}

void AppController::Subscription::Reset() noexcept {
    if (m_id == 0) return;
    if (auto state = m_state.lock()) state->Remove(m_id);
    m_state.reset();
    m_id = 0;
}

} // namespace apc::app
