#pragma once

#include <app/LatestStartupTaskRequestState.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <winrt/Windows.Foundation.h>

struct StartupTaskSnapshot {
    std::uint64_t Publication = 0;
    std::uint64_t Revision = 0;
    bool Enabled = false;
    bool Known = false;
    bool Busy = false;
    bool Failed = false;
};

class StartupTaskCoordinator : public std::enable_shared_from_this<StartupTaskCoordinator> {
public:
    using QueryOperation = std::function<winrt::Windows::Foundation::IAsyncOperation<bool>()>;
    using SetOperation = std::function<winrt::Windows::Foundation::IAsyncOperation<bool>(bool)>;
    using CommitActual = std::function<void(bool)>;
    using ChangedHandler = std::function<void(StartupTaskSnapshot const&)>;
    using HandlerToken = std::uint64_t;

    explicit StartupTaskCoordinator(CommitActual commitActual);
    StartupTaskCoordinator(QueryOperation queryOperation, SetOperation setOperation, CommitActual commitActual);
    ~StartupTaskCoordinator();

    StartupTaskCoordinator(StartupTaskCoordinator const&) = delete;
    StartupTaskCoordinator& operator=(StartupTaskCoordinator const&) = delete;

    void Refresh() noexcept;
    void RequestDesired(bool enabled) noexcept;
    [[nodiscard]] StartupTaskSnapshot Snapshot() const noexcept;
    [[nodiscard]] HandlerToken Subscribe(ChangedHandler handler);
    void Unsubscribe(HandlerToken token) noexcept;
    void Shutdown() noexcept;

private:
    void StartOperation(LatestStartupTaskRequestState::OperationToken operation) noexcept;
    winrt::fire_and_forget RunOperationAsync(LatestStartupTaskRequestState::OperationToken operation);
    void CompleteOperation(LatestStartupTaskRequestState::OperationToken operation,
                           bool known,
                           bool actual,
                           bool failed) noexcept;
    [[nodiscard]] std::vector<ChangedHandler> StoreSnapshot(StartupTaskSnapshot& snapshot) noexcept;
    static void NotifyHandlers(std::vector<ChangedHandler> const& handlers,
                               StartupTaskSnapshot const& snapshot) noexcept;

    QueryOperation m_queryOperation;
    SetOperation m_setOperation;
    std::shared_ptr<CommitActual> m_commitActual;
    LatestStartupTaskRequestState m_requests;
    std::recursive_mutex m_launchMutex;
    std::mutex m_requestMutex;
    mutable std::mutex m_mutex;
    StartupTaskSnapshot m_snapshot;
    std::unordered_map<HandlerToken, ChangedHandler> m_handlers;
    HandlerToken m_nextHandlerToken = 1;
    std::uint64_t m_nextPublication = 1;
    bool m_confirmedEnabled = false;
    bool m_confirmedKnown = false;
    bool m_stopping = false;
};
