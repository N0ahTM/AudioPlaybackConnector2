#include <pch.h>

#include <app/StartupTaskCoordinator.hpp>

#include <services/StartupTaskController.hpp>
#include <util/Util.hpp>

StartupTaskCoordinator::StartupTaskCoordinator(CommitActual commitActual)
    : StartupTaskCoordinator([] { return StartupTaskController::IsEnabledAsync(); },
                             [](bool enabled) { return StartupTaskController::SetEnabledAsync(enabled); },
                             std::move(commitActual)) {}

StartupTaskCoordinator::StartupTaskCoordinator(QueryOperation queryOperation,
                                               SetOperation setOperation,
                                               CommitActual commitActual)
    : m_queryOperation(std::move(queryOperation)), m_setOperation(std::move(setOperation)),
      m_commitActual(commitActual ? std::make_shared<CommitActual>(std::move(commitActual)) : nullptr) {
    if (!m_queryOperation || !m_setOperation) throw std::invalid_argument("startup task operations are required");
}

StartupTaskCoordinator::~StartupTaskCoordinator() {
    Shutdown();
}

void StartupTaskCoordinator::Refresh() noexcept {
    try {
        std::optional<LatestStartupTaskRequestState::OperationToken> operation;
        StartupTaskSnapshot snapshot;
        std::vector<ChangedHandler> handlers;
        {
            std::scoped_lock requestLock(m_requestMutex);
            auto request = m_requests.RequestRefresh();
            if (!request.Accepted || request.Coalesced) return;
            snapshot = Snapshot();
            snapshot.Revision = request.Revision;
            snapshot.Busy = true;
            snapshot.Failed = false;
            handlers = StoreSnapshot(snapshot);
            operation = request.OperationToStart;
        }
        NotifyHandlers(handlers, snapshot);
        if (operation) StartOperation(*operation);
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] refresh request ignored exception");
    }
}

void StartupTaskCoordinator::RequestDesired(bool enabled) noexcept {
    try {
        std::optional<LatestStartupTaskRequestState::OperationToken> operation;
        StartupTaskSnapshot snapshot;
        std::vector<ChangedHandler> handlers;
        {
            std::scoped_lock requestLock(m_requestMutex);
            auto request = m_requests.RequestDesired(enabled);
            if (!request.Accepted || request.Coalesced) return;
            snapshot = Snapshot();
            snapshot.Revision = request.Revision;
            snapshot.Enabled = enabled;
            snapshot.Known = false;
            snapshot.Busy = true;
            snapshot.Failed = false;
            handlers = StoreSnapshot(snapshot);
            operation = request.OperationToStart;
        }
        NotifyHandlers(handlers, snapshot);
        if (operation) StartOperation(*operation);
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] desired request ignored exception");
    }
}

StartupTaskSnapshot StartupTaskCoordinator::Snapshot() const noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        return m_snapshot;
    } catch (...) {
        return {};
    }
}

StartupTaskCoordinator::HandlerToken StartupTaskCoordinator::Subscribe(ChangedHandler handler) {
    if (!handler) return 0;
    HandlerToken token = 0;
    StartupTaskSnapshot snapshot;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping || m_nextHandlerToken == 0) return 0;
        token = m_nextHandlerToken++;
        m_handlers.emplace(token, handler);
        snapshot = m_snapshot;
    }
    try {
        handler(snapshot);
    } catch (...) {
        Unsubscribe(token);
        throw;
    }
    return token;
}

void StartupTaskCoordinator::Unsubscribe(HandlerToken token) noexcept {
    if (token == 0) return;
    try {
        std::scoped_lock lock(m_mutex);
        m_handlers.erase(token);
    } catch (...) {
    }
}

void StartupTaskCoordinator::Shutdown() noexcept {
    try {
        std::scoped_lock launchLock(m_launchMutex);
        std::scoped_lock requestLock(m_requestMutex);
        m_requests.Stop();
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_stopping = true;
        m_snapshot.Busy = false;
        m_handlers.clear();
        m_commitActual = nullptr;
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] shutdown ignored exception");
    }
}

void StartupTaskCoordinator::StartOperation(LatestStartupTaskRequestState::OperationToken operation) noexcept {
    try {
        std::scoped_lock launchLock(m_launchMutex);
        {
            std::scoped_lock lock(m_mutex);
            if (m_stopping) return;
        }
        RunOperationAsync(operation);
    } catch (...) {
        CompleteOperation(operation, false, false, true);
    }
}

winrt::fire_and_forget
StartupTaskCoordinator::RunOperationAsync(LatestStartupTaskRequestState::OperationToken operation) {
    auto lifetime = shared_from_this();
    bool known = false;
    bool actual = false;
    bool failed = false;

    if (operation.Kind == LatestStartupTaskRequestState::RequestKind::Desired) {
        try {
            static_cast<void>(co_await m_setOperation(operation.Desired));
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[StartupTaskCoordinator] set operation failed", ex);
            failed = true;
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[StartupTaskCoordinator] set operation failed", ex);
            failed = true;
        } catch (...) {
            util::DebugTraceUnknownException(L"[StartupTaskCoordinator] set operation failed");
            failed = true;
        }
    }

    try {
        actual = co_await m_queryOperation();
        known = true;
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[StartupTaskCoordinator] state query failed", ex);
        failed = true;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[StartupTaskCoordinator] state query failed", ex);
        failed = true;
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] state query failed");
        failed = true;
    }

    if (known) {
        failed = operation.Kind == LatestStartupTaskRequestState::RequestKind::Desired && actual != operation.Desired;
    }
    CompleteOperation(operation, known, actual, failed);
}

void StartupTaskCoordinator::CompleteOperation(LatestStartupTaskRequestState::OperationToken operation,
                                               bool known,
                                               bool actual,
                                               bool failed) noexcept {
    try {
        std::optional<LatestStartupTaskRequestState::OperationToken> nextOperation;
        StartupTaskSnapshot snapshot;
        std::vector<ChangedHandler> handlers;
        bool publish = false;
        {
            std::scoped_lock requestLock(m_requestMutex);
            auto const desiredReached = operation.Kind != LatestStartupTaskRequestState::RequestKind::Desired ||
                                        (known && actual == operation.Desired);
            auto completion = m_requests.Complete(operation, desiredReached);
            if (completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Stale) return;

            if (completion.Disposition == LatestStartupTaskRequestState::CompletionDisposition::Publish) {
                snapshot = Snapshot();
                snapshot.Revision = operation.Revision;
                snapshot.Busy = false;
                snapshot.Failed = failed || !known;

                std::shared_ptr<CommitActual> commit;
                {
                    std::scoped_lock lock(m_mutex);
                    if (m_stopping) return;
                    if (known) {
                        m_confirmedKnown = true;
                        m_confirmedEnabled = actual;
                    }
                    snapshot.Known = m_confirmedKnown;
                    snapshot.Enabled = m_confirmedEnabled;
                    commit = m_commitActual;
                }

                if (known && commit) {
                    try {
                        (*commit)(actual);
                    } catch (...) {
                        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] settings commit ignored exception");
                    }
                }
                handlers = StoreSnapshot(snapshot);
                publish = true;
            }
            nextOperation = completion.OperationToStart;
        }
        if (publish) NotifyHandlers(handlers, snapshot);
        if (nextOperation) StartOperation(*nextOperation);
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] completion ignored exception");
    }
}

std::vector<StartupTaskCoordinator::ChangedHandler>
StartupTaskCoordinator::StoreSnapshot(StartupTaskSnapshot& snapshot) noexcept {
    std::vector<ChangedHandler> handlers;
    try {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return handlers;
        if (m_nextPublication == 0) return handlers;
        snapshot.Publication = m_nextPublication++;
        m_snapshot = snapshot;
        handlers.reserve(m_handlers.size());
        for (auto const& [token, handler] : m_handlers) {
            static_cast<void>(token);
            handlers.push_back(handler);
        }
    } catch (...) {
        util::DebugTraceUnknownException(L"[StartupTaskCoordinator] handler snapshot allocation ignored exception");
    }
    return handlers;
}

void StartupTaskCoordinator::NotifyHandlers(std::vector<ChangedHandler> const& handlers,
                                            StartupTaskSnapshot const& snapshot) noexcept {
    for (auto const& handler : handlers) {
        try {
            handler(snapshot);
        } catch (...) {
            util::DebugTraceUnknownException(L"[StartupTaskCoordinator] handler ignored exception");
        }
    }
}
