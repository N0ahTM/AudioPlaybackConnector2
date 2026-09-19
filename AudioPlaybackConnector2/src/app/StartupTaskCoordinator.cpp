#include <pch.h>

#include <app/StartupTaskCoordinator.hpp>
#include <app/LatestStartupTaskRequestState.hpp>
#include <services/StartupTaskController.hpp>
#include <util/Util.hpp>
#include <winrt/Windows.ApplicationModel.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Serial Startup Task Owner /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct StartupTaskCoordinator::State : std::enable_shared_from_this<State> {
    using OperationToken = LatestStartupTaskRequestState::OperationToken;
    struct Entry {
        ChangedHandler Handler;
        bool Active = true;
        std::optional<std::uint64_t> LastPublication;
    };

    State(QueryOperation query, SetOperation set, CommitActual commit)
        : Query(std::move(query)), Set(std::move(set)), Commit(std::move(commit)) {
        if (!Query || !Set) throw std::invalid_argument("startup task operations are required");
    }

    // The mutex protects queue admission, the published snapshot, and subscription lifetime only.
    // Request policy and confirmations belong to the single drainer. No foreign call runs under Mutex.
    bool Post(std::function<void()> work) {
        {
            std::lock_guard lock(Mutex);
            if (Stopped) return false;
            Pending.push_back(std::move(work));
            if (Draining) return true;
            Draining = true;
            DrainThread = std::this_thread::get_id();
        }
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard lock(Mutex);
                if (Pending.empty()) {
                    Draining = false;
                    DrainThread = {};
                    Changed.notify_all();
                    return true;
                }
                next = std::move(Pending.front());
                Pending.pop_front();
            }
            try {
                if (!IsStopped()) next();
            } catch (...) {
                util::DebugTraceUnknownException(L"[StartupTaskCoordinator] owner operation failed");
            }
        }
    }

    bool IsStopped() const {
        std::lock_guard lock(Mutex);
        return Stopped;
    }

    StartupTaskSnapshot Snapshot() const {
        std::lock_guard lock(Mutex);
        return Published;
    }

    void Deliver(std::shared_ptr<Entry> const& entry, StartupTaskSnapshot const& snapshot) {
        {
            std::lock_guard lock(Mutex);
            if (Stopped || !entry->Active ||
                (entry->LastPublication && snapshot.Publication <= *entry->LastPublication))
                return;
            entry->LastPublication = snapshot.Publication;
            Delivering = entry;
        }
        try {
            entry->Handler(snapshot);
        } catch (...) {
            util::DebugTraceUnknownException(L"[StartupTaskCoordinator] observer failed");
        }
        {
            std::lock_guard lock(Mutex);
            Delivering.reset();
        }
        Changed.notify_all();
    }

    void Publish(StartupTaskSnapshot snapshot) {
        std::vector<std::shared_ptr<Entry>> recipients;
        {
            std::lock_guard lock(Mutex);
            if (Stopped) return;
            snapshot.Publication = Published.Publication + 1;
            Published = snapshot;
            for (auto const& [id, entry] : Handlers)
                recipients.push_back(entry);
        }
        for (auto const& entry : recipients)
            Deliver(entry, snapshot);
    }

    HandlerToken Subscribe(ChangedHandler handler) {
        if (!handler) return 0;
        auto entry = std::make_shared<Entry>(std::move(handler));
        HandlerToken token;
        {
            std::lock_guard lock(Mutex);
            if (Stopped || NextToken == 0) return 0;
            token = NextToken++;
            Handlers.emplace(token, entry);
        }
        auto self = shared_from_this();
        Post([self, entry] { self->Deliver(entry, self->Snapshot()); });
        return token;
    }

    void Unsubscribe(HandlerToken token) {
        std::shared_ptr<Entry> removed;
        std::unique_lock lock(Mutex);
        auto found = Handlers.find(token);
        if (found == Handlers.end()) return;
        removed = std::move(found->second);
        Handlers.erase(found);
        removed->Active = false;
        if (DrainThread != std::this_thread::get_id()) Changed.wait(lock, [&] { return Delivering != removed; });
        lock.unlock();
    }

    void Stop() {
        std::deque<std::function<void()>> abandoned;
        std::unordered_map<HandlerToken, std::shared_ptr<Entry>> removed;
        {
            std::unique_lock lock(Mutex);
            Stopped = true;
            if (Published.Busy) {
                Published.Busy = false;
                ++Published.Publication;
            }
            abandoned.swap(Pending);
            removed.swap(Handlers);
            if (DrainThread != std::this_thread::get_id()) Changed.wait(lock, [&] { return !Draining; });
        }
        // Captures may release owners or subscriptions; destruction must also be outside the mutex.
    }

    void Request(std::optional<bool> desired) {
        auto request = desired ? Requests.RequestDesired(*desired) : Requests.RequestRefresh();
        if (!request.Accepted || request.Coalesced) return;
        auto snapshot = Snapshot();
        snapshot.Revision = request.Revision;
        snapshot.Busy = true;
        snapshot.Failed = false;
        if (desired) {
            snapshot.Enabled = *desired;
            snapshot.Known = false;
        }
        Publish(snapshot);
        if (request.OperationToStart && !IsStopped()) Run(shared_from_this(), *request.OperationToStart);
    }

    static winrt::fire_and_forget QueryActual(std::shared_ptr<State> self, OperationToken operation) {
        bool known = false;
        bool actual = false;
        try {
            actual = co_await self->Query();
            known = true;
        } catch (...) {
            util::DebugTraceUnknownException(L"[StartupTaskCoordinator] query operation failed");
        }
        auto failed = !known || (operation.Kind == LatestStartupTaskRequestState::RequestKind::Desired &&
                                 actual != operation.Desired);
        self->Post([self, operation, known, actual, failed] { self->Complete(operation, known, actual, failed); });
    }

    static winrt::fire_and_forget SetDesired(std::shared_ptr<State> self, OperationToken operation) {
        try {
            static_cast<void>(co_await self->Set(operation.Desired));
        } catch (...) {
            // Even a failed set is followed by a query of the authoritative OS state.
            util::DebugTraceUnknownException(L"[StartupTaskCoordinator] set operation failed");
        }
        self->Post([self, operation] { QueryActual(self, operation); });
    }

    static void Run(std::shared_ptr<State> self, OperationToken operation) {
        if (operation.Kind == LatestStartupTaskRequestState::RequestKind::Desired)
            SetDesired(std::move(self), operation);
        else
            QueryActual(std::move(self), operation);
    }

    void Complete(OperationToken operation, bool known, bool actual, bool failed) {
        auto desiredReached = operation.Kind != LatestStartupTaskRequestState::RequestKind::Desired ||
                              (known && actual == operation.Desired);
        auto completion = Requests.Complete(operation, desiredReached);
        if (completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish) {
            if (known) {
                ConfirmedKnown = true;
                ConfirmedEnabled = actual;
                if (Commit && !IsStopped()) {
                    try {
                        Commit(actual);
                    } catch (...) {
                        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] settings commit failed");
                    }
                }
            }
            auto snapshot = Snapshot();
            snapshot.Revision = operation.Revision;
            snapshot.Known = ConfirmedKnown;
            snapshot.Enabled = ConfirmedEnabled;
            snapshot.Busy = false;
            snapshot.Failed = failed || !known;
            Publish(snapshot);
        }
        if (completion.OperationToStart && !IsStopped()) Run(shared_from_this(), *completion.OperationToStart);
    }

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    QueryOperation Query;
    SetOperation Set;
    CommitActual Commit;
    LatestStartupTaskRequestState Requests;
    bool ConfirmedEnabled = false;
    bool ConfirmedKnown = false;
    mutable std::mutex Mutex;
    std::condition_variable Changed;
    StartupTaskSnapshot Published;
    std::deque<std::function<void()>> Pending;
    std::unordered_map<HandlerToken, std::shared_ptr<Entry>> Handlers;
    std::shared_ptr<Entry> Delivering;
    HandlerToken NextToken = 1;
    std::thread::id DrainThread;
    bool Draining = false;
    bool Stopped = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

StartupTaskCoordinator::StartupTaskCoordinator(CommitActual commitActual)
    : StartupTaskCoordinator([] { return StartupTaskController::IsEnabledAsync(); },
                             [](bool enabled) { return StartupTaskController::SetEnabledAsync(enabled); },
                             std::move(commitActual)) {}

StartupTaskCoordinator::StartupTaskCoordinator(QueryOperation query, SetOperation set, CommitActual commit)
    : m_state(std::make_shared<State>(std::move(query), std::move(set), std::move(commit))) {}

StartupTaskCoordinator::~StartupTaskCoordinator() {
    Shutdown();
}

bool StartupTaskCoordinator::Refresh() noexcept {
    auto state = m_state;
    return state->Post([state] { state->Request(std::nullopt); });
}

bool StartupTaskCoordinator::RequestDesired(bool enabled) noexcept {
    auto state = m_state;
    return state->Post([state, enabled] { state->Request(enabled); });
}

StartupTaskSnapshot StartupTaskCoordinator::Snapshot() const noexcept {
    return m_state->Snapshot();
}
StartupTaskCoordinator::HandlerToken StartupTaskCoordinator::Subscribe(ChangedHandler handler) {
    auto state = m_state;
    return state->Subscribe(std::move(handler));
}
void StartupTaskCoordinator::Unsubscribe(HandlerToken token) noexcept {
    m_state->Unsubscribe(token);
}
void StartupTaskCoordinator::Shutdown() noexcept {
    m_state->Stop();
}
