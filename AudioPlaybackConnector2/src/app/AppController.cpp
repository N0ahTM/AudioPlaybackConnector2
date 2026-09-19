#include <app/AppController.hpp>
#include <core/DeviceService.hpp>

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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Ordered Event Delivery ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct AppController::EventState {
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

    void Close() noexcept {
        std::vector<std::shared_ptr<Entry>> removed;
        std::deque<Delivery> discarded;
        {
            std::unique_lock lock(Mutex);
            Closed = true;
            for (auto const& entry : Handlers)
                entry->Active = false;
            removed.swap(Handlers);
            discarded.swap(Pending);
            if (DrainThread != std::this_thread::get_id()) Changed.wait(lock, [this] { return !Draining; });
        }
    }

    void Publish(AppEvent const& event, std::optional<DeviceFactPublicationFence::Token> token = {}) {
        {
            std::lock_guard lock(Mutex);
            if (Closed || NextRevision == 0) return;
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
        }
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

    DeviceFactPublicationFence DeviceFence;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<std::shared_ptr<Entry>> Handlers;
    std::deque<Delivery> Pending;
    SubscriptionId NextId = 1;
    std::uint64_t NextRevision = 1;
    std::thread::id DrainThread;
    bool Draining = false;
    bool Closed = false;
};

AppController::AppController(std::shared_ptr<SettingsStore> settings,
                             std::shared_ptr<apc::device::DeviceService> devices,
                             std::weak_ptr<AppPresentation> presentation)
    : m_settings(std::move(settings)), m_presentation(std::move(presentation)),
      m_eventState(std::make_shared<EventState>()), m_devices(std::move(devices)) {
    if (!m_settings || !m_devices) throw std::invalid_argument("application owners are required");
    std::weak_ptr<EventState> weak = m_eventState;
    m_deviceSubscription = m_devices->Subscribe([weak](apc::device::DeviceFact const& fact) {
        if (auto state = weak.lock()) state->ObserveDevice(fact);
    });
}

AppController::~AppController() {
    Shutdown();
    if (m_devices && m_deviceSubscription) m_devices->Unsubscribe(m_deviceSubscription);
}

void AppController::Shutdown() noexcept {
    {
        std::unique_lock lock(m_stateMutex);
        // Every caller drains admission, including concurrent shutdown calls.
        if (m_running) {
            m_running = false;
            AdvanceGeneration(m_generation);
        }
        m_noActiveCalls.wait(lock, [this] { return m_activeCalls == 0; });
    }
    // Closing delivery may wait for a foreign observer; it must run unlocked.
    m_eventState->Close();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UI Actions ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::ShowDevicePicker(DevicePickerOpenMode mode, AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ShowDevicePicker, {}, {}, mode}, context);
}

AppResult AppController::ShowSettings(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ShowSettings, {}, {}}, context);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Actions ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::Connect(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::Connect, std::move(target), {}}, context);
}

AppResult AppController::Disconnect(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::Disconnect, std::move(target), {}}, context);
}

AppResult AppController::Reconnect(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::Reconnect, std::move(target), {}}, context);
}

AppResult AppController::ToggleDefault(AppCommandContext context) const {
    return Toggle(DeviceSelector::Default(), context);
}

AppResult AppController::Toggle(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::ToggleLast, std::move(target), {}}, context);
}

AppResult AppController::DisconnectAll(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::DisconnectAll, {}, {}}, context);
}

AppResult AppController::ReconnectAll(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ReconnectAll, {}, {}}, context);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Settings ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::SetDefault(std::wstring_view deviceId) const {
    return Execute({AppCommandKind::SetDefault, DeviceSelector::ById(deviceId), {}});
}

AppResult AppController::ClearDefault(AppCommandContext context) const {
    return Execute({AppCommandKind::ClearDefault, {}, {}}, context);
}

AppResult AppController::SetAlias(std::wstring_view deviceId, std::wstring_view alias) const {
    if (alias.empty()) return ClearAlias(deviceId);
    return Execute({AppCommandKind::SetAlias, DeviceSelector::ById(deviceId), std::wstring(alias)});
}

AppResult AppController::ClearAlias(std::wstring_view deviceId) const {
    return Execute({AppCommandKind::ClearAlias, DeviceSelector::ById(deviceId), {}});
}

AppResult AppController::SetDefault(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::SetDefault, std::move(target), {}}, context);
}

AppResult AppController::SetAlias(DeviceSelector target, std::wstring_view alias, AppCommandContext context) const {
    return Execute({AppCommandKind::SetAlias, std::move(target), std::wstring(alias)}, context);
}

AppResult AppController::ClearAlias(DeviceSelector target, AppCommandContext context) const {
    return Execute({AppCommandKind::ClearAlias, std::move(target), {}}, context);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Queries ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AppResult AppController::ListDevices(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ListDevices, {}, {}}, context);
}

AppResult AppController::Status(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::Status, {}, {}}, context);
}

AppResult AppController::ShowDefault(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ShowDefault, {}, {}}, context);
}

AppResult AppController::ListAliases(AppCommandContext context) const noexcept {
    return Execute({AppCommandKind::ListAliases, {}, {}}, context);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Subscriptions ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
