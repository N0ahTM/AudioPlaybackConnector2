#pragma once

#include <cstdint>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

class LatestServiceLifecycleState {
public:
    enum class DesiredState { Stopped, Running };

    struct OperationToken {
        std::uint64_t Revision = 0;
        DesiredState State = DesiredState::Stopped;
        bool ClearState = false;

        bool operator==(OperationToken const&) const = default;
    };

    struct RequestResult {
        bool Accepted = false;
        bool Coalesced = false;
        std::optional<OperationToken> OperationToStart;
    };

    [[nodiscard]] RequestResult RequestStart() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_permanentlyStopped) return {};
        return RequestLocked(DesiredState::Running, false);
    }

    [[nodiscard]] RequestResult RequestStop(bool permanently = false) noexcept {
        std::scoped_lock lock(m_mutex);
        m_permanentlyStopped = m_permanentlyStopped || permanently;
        return RequestLocked(DesiredState::Stopped, m_permanentlyStopped);
    }

    [[nodiscard]] std::optional<OperationToken> Complete(OperationToken const& operation) noexcept {
        std::scoped_lock lock(m_mutex);
        if (!m_inFlight || *m_inFlight != operation) return std::nullopt;

        if (m_latestRevision == operation.Revision && m_latestState == operation.State &&
            m_latestClearState == operation.ClearState) {
            m_inFlight.reset();
            m_executorThread = {};
            m_idle.notify_all();
            return std::nullopt;
        }

        m_inFlight = OperationToken{m_latestRevision, m_latestState, m_latestClearState};
        return m_inFlight;
    }

    [[nodiscard]] bool BeginExecution(OperationToken const& operation) noexcept {
        std::scoped_lock lock(m_mutex);
        if (!m_inFlight || *m_inFlight != operation || m_executorThread != std::thread::id{}) return false;
        m_executorThread = std::this_thread::get_id();
        return true;
    }

    void WaitForIdle() {
        std::unique_lock lock(m_mutex);
        if (m_executorThread == std::this_thread::get_id()) return;
        m_idle.wait(lock, [this] { return !m_inFlight; });
    }

    [[nodiscard]] std::optional<OperationToken> InFlight() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_inFlight;
    }

    [[nodiscard]] bool PermanentlyStopped() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_permanentlyStopped;
    }

private:
    [[nodiscard]] RequestResult RequestLocked(DesiredState state, bool clearState) noexcept {
        if (m_inFlight && m_latestState == state && m_latestClearState == clearState) {
            return {true, true, std::nullopt};
        }

        if (m_latestRevision != std::numeric_limits<std::uint64_t>::max()) {
            ++m_latestRevision;
        } else {
            m_permanentlyStopped = true;
            state = DesiredState::Stopped;
            clearState = true;
        }
        m_latestState = state;
        m_latestClearState = clearState;
        if (m_inFlight) return {true, false, std::nullopt};

        m_inFlight = OperationToken{m_latestRevision, m_latestState, m_latestClearState};
        return {true, false, m_inFlight};
    }

    mutable std::mutex m_mutex;
    std::uint64_t m_latestRevision = 0;
    DesiredState m_latestState = DesiredState::Stopped;
    bool m_latestClearState = false;
    bool m_permanentlyStopped = false;
    std::optional<OperationToken> m_inFlight;
    std::thread::id m_executorThread;
    std::condition_variable m_idle;
};
