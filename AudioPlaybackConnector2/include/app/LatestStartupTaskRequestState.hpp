#pragma once

#include <cstdint>
#include <limits>
#include <optional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Latest Startup Task Request State /////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Only StartupTaskCoordinator's serial drainer accesses this policy. Admission
// and shutdown belong to the coordinator, not to a second lock or stop path here.
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
        if (m_revisionExhausted) return {m_revision, false, false, std::nullopt};

        if (m_latestRequest && m_latestRequest->Kind == RequestKind::Desired && m_latestRequest->Desired == desired) {
            return {m_latestRequest->Revision, true, true, std::nullopt};
        }

        return RecordRequest(RequestKind::Desired, desired);
    }

    [[nodiscard]] RequestResult RequestRefresh() noexcept {
        if (m_revisionExhausted) return {m_revision, false, false, std::nullopt};

        if (!m_latestSettled && m_latestRequest) {
            return {m_latestRequest->Revision, true, true, std::nullopt};
        }

        return RecordRequest(RequestKind::Refresh, false);
    }

    [[nodiscard]] CompletionResult Complete(OperationToken operation, bool retainForCoalescing = true) noexcept {
        if (m_revisionExhausted || !m_inFlight || *m_inFlight != operation) return {};

        m_inFlight.reset();
        if (m_latestRequest && m_latestRequest->Revision == operation.Revision) {
            m_latestSettled = retainForCoalescing;
            if (!retainForCoalescing) m_latestRequest.reset();
            return {CompletionDisposition::Publish, std::nullopt};
        }

        if (!m_latestRequest) return {};
        m_inFlight = *m_latestRequest;
        return {CompletionDisposition::Superseded, m_inFlight};
    }

private:
    [[nodiscard]] RequestResult RecordRequest(RequestKind kind, bool desired) noexcept {
        if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
            m_revisionExhausted = true;
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

    std::uint64_t m_revision = 0;
    std::optional<OperationToken> m_latestRequest;
    std::optional<OperationToken> m_inFlight;
    bool m_latestSettled = false;
    bool m_revisionExhausted = false;
};
