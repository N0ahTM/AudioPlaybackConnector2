#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

class LatestStartupTaskRequestState {
public:
    enum class RequestKind { Refresh, Desired };

    struct OperationToken {
        std::uint64_t Revision = 0;
        RequestKind Kind = RequestKind::Refresh;
        bool Desired = false;

        bool operator==(OperationToken const&) const = default;
    };

    struct RequestResult {
        std::uint64_t Revision = 0;
        bool Accepted = false;
        bool Coalesced = false;
        std::optional<OperationToken> OperationToStart;
    };

    enum class CompletionDisposition { Stale, Publish, Superseded };

    struct CompletionResult {
        CompletionDisposition Disposition = CompletionDisposition::Stale;
        std::optional<OperationToken> OperationToStart;
    };

    [[nodiscard]] RequestResult RequestDesired(bool desired) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped) return {m_revision, false, false, std::nullopt};

        if (m_latestRequest && m_latestRequest->Kind == RequestKind::Desired && m_latestRequest->Desired == desired) {
            return {m_latestRequest->Revision, true, true, std::nullopt};
        }

        return RecordRequestLocked(RequestKind::Desired, desired);
    }

    [[nodiscard]] RequestResult RequestRefresh() noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped) return {m_revision, false, false, std::nullopt};

        if (!m_latestSettled && m_latestRequest && m_latestRequest->Kind == RequestKind::Refresh) {
            return {m_latestRequest->Revision, true, true, std::nullopt};
        }

        return RecordRequestLocked(RequestKind::Refresh, false);
    }

    [[nodiscard]] CompletionResult Complete(OperationToken operation) noexcept {
        std::scoped_lock lock(m_mutex);
        if (m_stopped || !m_inFlight || *m_inFlight != operation) return {};

        m_inFlight.reset();
        if (m_latestRequest && m_latestRequest->Revision == operation.Revision) {
            m_latestSettled = true;
            return {CompletionDisposition::Publish, std::nullopt};
        }

        if (!m_latestRequest) return {};
        m_inFlight = *m_latestRequest;
        return {CompletionDisposition::Superseded, m_inFlight};
    }

    void Stop() noexcept {
        std::scoped_lock lock(m_mutex);
        m_stopped = true;
        m_latestSettled = false;
        m_latestRequest.reset();
        m_inFlight.reset();
    }

    [[nodiscard]] std::uint64_t Revision() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_revision;
    }

    [[nodiscard]] std::optional<OperationToken> InFlight() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_inFlight;
    }

    [[nodiscard]] bool Stopped() const noexcept {
        std::scoped_lock lock(m_mutex);
        return m_stopped;
    }

private:
    [[nodiscard]] RequestResult RecordRequestLocked(RequestKind kind, bool desired) noexcept {
        if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
            m_stopped = true;
            m_latestSettled = false;
            m_latestRequest.reset();
            m_inFlight.reset();
            return {m_revision, false, false, std::nullopt};
        }

        OperationToken request{++m_revision, kind, desired};
        m_latestRequest = request;
        m_latestSettled = false;
        if (m_inFlight) return {request.Revision, true, false, std::nullopt};

        m_inFlight = request;
        return {request.Revision, true, false, m_inFlight};
    }

    mutable std::mutex m_mutex;
    std::uint64_t m_revision = 0;
    std::optional<OperationToken> m_latestRequest;
    std::optional<OperationToken> m_inFlight;
    bool m_latestSettled = false;
    bool m_stopped = false;
};
