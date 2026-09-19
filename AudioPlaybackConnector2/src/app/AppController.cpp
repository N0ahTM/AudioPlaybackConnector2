#include <app/AppController.hpp>

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

    void Publish(AppEvent const& event) {
        {
            std::lock_guard lock(Mutex);
            if (Closed || NextRevision == 0) return;
            Pending.push_back({{NextRevision, event}, Handlers});
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

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

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

AppController::AppController(Executor executor, SnapshotProvider snapshotProvider)
    : m_executor(std::move(executor)), m_snapshotProvider(std::move(snapshotProvider)),
      m_eventState(std::make_shared<EventState>()) {
    if (!m_executor) throw std::invalid_argument("app controller executor is required");
    if (!m_snapshotProvider) throw std::invalid_argument("app controller snapshot provider is required");
}

AppController::~AppController() {
    m_eventState->Close();
}

AppResult AppController::Execute(AppCommand command, AppCommandContext context) const noexcept {
    AppResult result;
    result.Command = command.Kind;

    // Keep preflight precedence stable for adapters: malformed input wins,
    // followed by explicit cancellation, then the command deadline.
    if (!command.IsWellFormed()) {
        result.Code = AppResultCode::InvalidInput;
        return result;
    }
    if (context.IsCancellationRequested()) {
        result.Code = AppResultCode::Cancelled;
        return result;
    }
    if (context.IsExpired(AppCommandContext::Clock::now())) {
        result.Code = AppResultCode::TimedOut;
        return result;
    }

    try {
        result = m_executor(command, context);
        result.Command = command.Kind;
        result.DispatchPhase = AppDispatchPhase::Started;
        return result;
    } catch (...) {
        result = {};
        result.Code = AppResultCode::InternalError;
        result.Command = command.Kind;
        result.DispatchPhase = AppDispatchPhase::Started;
        return result;
    }
}

AppSnapshot AppController::Snapshot() const noexcept {
    try {
        return m_snapshotProvider();
    } catch (...) {
        AppSnapshot unavailable;
        unavailable.IsRunning = false;
        return unavailable;
    }
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
