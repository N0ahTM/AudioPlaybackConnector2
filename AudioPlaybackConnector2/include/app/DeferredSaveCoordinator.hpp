#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

class DeferredSaveCoordinator {
public:
    struct WorkerToken {
        std::uint64_t Value = 0;
        bool operator==(WorkerToken const&) const = default;
    };

    struct AttemptToken {
        WorkerToken Worker;
        std::uint64_t Generation = 0;
        bool operator==(AttemptToken const&) const = default;
    };

    struct RequestResult {
        std::uint64_t Generation = 0;
        std::optional<WorkerToken> WorkerToStart;
    };

    struct ExternalSaveToken {
        std::uint64_t Generation = 0;
    };

    enum class Completion { Stop, ContinueAfterDebounce, RetryWithBackoff, Stale };

    [[nodiscard]] RequestResult MarkDirty() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped) return {m_generation, std::nullopt};

        ++m_generation;
        m_dirty = true;
        if (m_activeWorker) return {m_generation, std::nullopt};

        WorkerToken worker{++m_workerEpoch};
        m_activeWorker = worker;
        return {m_generation, worker};
    }

    [[nodiscard]] std::optional<AttemptToken> BeginAttempt(WorkerToken worker) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped || !m_activeWorker || *m_activeWorker != worker || m_attemptActive) return std::nullopt;

        m_attemptActive = true;
        m_attemptGeneration = m_generation;
        m_dirty = false;
        return AttemptToken{worker, m_attemptGeneration};
    }

    [[nodiscard]] Completion CompleteAttempt(AttemptToken attempt, bool succeeded) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped || !m_activeWorker || *m_activeWorker != attempt.Worker || !m_attemptActive ||
            m_attemptGeneration != attempt.Generation) {
            return Completion::Stale;
        }

        m_attemptActive = false;
        if (!succeeded) {
            m_dirty = true;
            return Completion::RetryWithBackoff;
        }
        if (m_dirty || m_generation != attempt.Generation) {
            return Completion::ContinueAfterDebounce;
        }

        m_activeWorker.reset();
        return Completion::Stop;
    }

    [[nodiscard]] bool AbandonWorker(WorkerToken worker) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped || !m_activeWorker || *m_activeWorker != worker) return false;

        m_dirty = true;
        m_attemptActive = false;
        m_activeWorker.reset();
        ++m_workerEpoch;
        return true;
    }

    [[nodiscard]] ExternalSaveToken BeginExternalSave() const noexcept {
        std::scoped_lock lock(m_mutex);
        return ExternalSaveToken{m_generation};
    }

    [[nodiscard]] bool CompleteExternalSave(ExternalSaveToken token, bool savedAndClean) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped || !savedAndClean || token.Generation != m_generation) return false;
        m_dirty = false;
        m_attemptActive = false;
        m_activeWorker.reset();
        ++m_workerEpoch;
        return true;
    }

    void Cancel() noexcept {
        std::scoped_lock lock(m_mutex);
        m_stopped = true;
        m_dirty = false;
        m_attemptActive = false;
        m_activeWorker.reset();
        ++m_workerEpoch;
    }

    [[nodiscard]] std::uint64_t Generation() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_generation;
    }

    [[nodiscard]] bool WorkerActive() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_activeWorker.has_value();
    }

private:
    mutable std::mutex m_mutex;
    std::uint64_t m_generation = 0;
    std::uint64_t m_workerEpoch = 0;
    std::uint64_t m_attemptGeneration = 0;
    std::optional<WorkerToken> m_activeWorker;
    bool m_dirty = false;
    bool m_attemptActive = false;
    bool m_stopped = false;
};
