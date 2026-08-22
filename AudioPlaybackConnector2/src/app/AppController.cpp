#include <app/AppController.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace apc::app {

struct AppController::EventState {
    struct Entry {
        Entry(SubscriptionId id, EventHandler handler) : Id(id), Handler(std::move(handler)) {}

        SubscriptionId Id;
        EventHandler Handler;
        std::atomic_bool Active = true;
    };

    void Remove(SubscriptionId id) noexcept {
        try {
            std::scoped_lock lock(Mutex);
            auto const entry =
                std::ranges::find_if(Handlers, [id](auto const& candidate) { return candidate->Id == id; });
            if (entry == Handlers.end()) return;
            (*entry)->Active.store(false, std::memory_order_release);
            Handlers.erase(entry);
        } catch (...) {
        }
    }

    mutable std::mutex Mutex;
    std::vector<std::shared_ptr<Entry>> Handlers;
    SubscriptionId NextId = 1;
};

AppController::AppController(Executor executor, SnapshotProvider snapshotProvider)
    : m_executor(std::move(executor)), m_snapshotProvider(std::move(snapshotProvider)),
      m_eventState(std::make_shared<EventState>()) {
    if (!m_executor) throw std::invalid_argument("app controller executor is required");
    if (!m_snapshotProvider) throw std::invalid_argument("app controller snapshot provider is required");
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
        return result;
    } catch (...) {
        result = {};
        result.Code = AppResultCode::InternalError;
        result.Command = command.Kind;
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

AppController::Subscription AppController::Subscribe(EventHandler handler) {
    if (!handler) return {};

    auto const state = m_eventState;
    if (!state) return {};

    std::scoped_lock lock(state->Mutex);
    if (state->NextId == 0) return {};
    auto const id = state->NextId++;
    state->Handlers.push_back(std::make_shared<EventState::Entry>(id, std::move(handler)));
    return Subscription(state, id);
}

void AppController::Publish(AppEvent const& event) const noexcept {
    auto const state = m_eventState;
    if (!state) return;

    std::vector<std::shared_ptr<EventState::Entry>> handlers;
    try {
        {
            std::scoped_lock lock(state->Mutex);
            handlers = state->Handlers;
        }
    } catch (...) {
        return;
    }

    for (auto const& entry : handlers) {
        if (!entry->Active.load(std::memory_order_acquire)) continue;
        try {
            entry->Handler(event);
        } catch (...) {
            // One observer cannot prevent the remaining observers from seeing
            // the fact or turn a presentation failure into a command failure.
        }
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
