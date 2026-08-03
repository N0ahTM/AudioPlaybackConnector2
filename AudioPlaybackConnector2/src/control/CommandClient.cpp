#include <control/CommandClient.hpp>

namespace apc::control::client {
namespace {
SendResult ReplayIndeterminate(Transport& transport,
                               Request const& request,
                               Response& response,
                               ServerIdentityPtr const& server,
                               std::uint64_t overallDeadline) {
    if (!server) return SendResult::Indeterminate;
    constexpr std::size_t c_maxReplayAttempts = 2;
    for (std::size_t attempt = 0; attempt < c_maxReplayAttempts; ++attempt) {
        if (RemainingWait(overallDeadline) == 0) break;
        ServerIdentityPtr ignored;
        const auto result = transport.TrySendOnce(request, response, 2000, overallDeadline, ignored, server);
        if (result == AttemptResult::Complete) return SendResult::Complete;
        if (result == AttemptResult::ServerChanged || result == AttemptResult::Rejected) break;
    }
    return SendResult::Indeterminate;
}
} // namespace

SendResult SendRequest(Transport& transport,
                       Request const& request,
                       Response& response,
                       DWORD initialWaitMs,
                       DWORD launchWaitMs,
                       DWORD overallTimeoutMs) {
    if (!IsRequestValid(request)) return SendResult::InvalidRequest;

    const auto overallDeadline = DeadlineAfter(std::max<DWORD>(1, overallTimeoutMs));
    ServerIdentityPtr server;
    ServerIdentityPtr noExpectedServer;
    const auto firstAttempt =
        transport.TrySendOnce(request, response, initialWaitMs, overallDeadline, server, noExpectedServer);
    if (firstAttempt == AttemptResult::Complete) return SendResult::Complete;
    if (firstAttempt == AttemptResult::Indeterminate) {
        return ReplayIndeterminate(transport, request, response, server, overallDeadline);
    }
    if (firstAttempt == AttemptResult::ServerChanged) return SendResult::Unavailable;

    if (request.Command == CommandType::Status || RemainingWait(overallDeadline) == 0 ||
        !transport.LaunchPackagedApp()) {
        return SendResult::Unavailable;
    }

    ServerIdentityPtr launchedServer;
    const auto launchedAttempt =
        transport.TrySendOnce(request, response, launchWaitMs, overallDeadline, launchedServer, noExpectedServer);
    if (launchedAttempt == AttemptResult::Complete) return SendResult::Complete;
    if (launchedAttempt == AttemptResult::Indeterminate) {
        return ReplayIndeterminate(transport, request, response, launchedServer, overallDeadline);
    }
    return SendResult::Unavailable;
}

} // namespace apc::control::client
