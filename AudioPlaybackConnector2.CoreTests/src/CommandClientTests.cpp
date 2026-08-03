#include <control/CommandClient.hpp>

#include <deque>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class Identity final : public apc::control::client::ServerIdentity {};

class FakeTransport final : public apc::control::client::Transport {
public:
    struct Step {
        apc::control::client::AttemptResult Result;
        apc::control::client::ServerIdentityPtr Identity;
        apc::control::ExitCode ResponseCode = apc::control::ExitCode::Success;
        bool ExpireOverallDeadline = false;
    };

    apc::control::client::AttemptResult
    TrySendOnce(apc::control::Request const& request,
                apc::control::Response& response,
                DWORD waitMs,
                std::uint64_t overallDeadline,
                apc::control::client::ServerIdentityPtr& observedServer,
                apc::control::client::ServerIdentityPtr const& expectedServer) override {
        ++Attempts;
        Waits.push_back(waitMs);
        Deadlines.push_back(overallDeadline);
        Correlations.push_back(request.CorrelationId);
        if (Steps.empty()) return apc::control::client::AttemptResult::NotConnected;
        auto step = std::move(Steps.front());
        Steps.pop_front();
        if (step.ExpireOverallDeadline) {
            while (apc::control::RemainingWait(overallDeadline) != 0)
                Sleep(1);
        }
        if (expectedServer && expectedServer != step.Identity) {
            SawUnexpectedReplayTarget = true;
            return apc::control::client::AttemptResult::ServerChanged;
        }
        observedServer = step.Identity;
        if (step.Result == apc::control::client::AttemptResult::Complete) {
            response = {step.ResponseCode, L"response", request.CorrelationId};
        }
        return step.Result;
    }

    bool LaunchPackagedApp() override {
        ++Launches;
        return LaunchSucceeds;
    }

    std::deque<Step> Steps;
    std::vector<std::uint64_t> Deadlines;
    std::vector<DWORD> Waits;
    std::vector<apc::control::CorrelationId> Correlations;
    int Attempts = 0;
    int Launches = 0;
    bool LaunchSucceeds = true;
    bool SawUnexpectedReplayTarget = false;
};

apc::control::Request Request(apc::control::CommandType command = apc::control::CommandType::Show) {
    apc::control::Request request;
    request.Command = command;
    request.CorrelationId = {1, 2};
    return request;
}

void TestInvalidAndStatusNeverLaunch() {
    FakeTransport transport;
    apc::control::Response response;
    auto invalid = Request();
    invalid.CorrelationId = {};
    Check(apc::control::client::SendRequest(transport, invalid, response) ==
                  apc::control::client::SendResult::InvalidRequest &&
              transport.Attempts == 0,
          "invalid requests must be rejected before opening a pipe");

    auto maximum = Request(apc::control::CommandType::Connect);
    maximum.Target = apc::control::TargetKind::Name;
    maximum.Payload.assign(apc::control::c_maxPayloadBytes / sizeof(wchar_t), L'x');
    Check(apc::control::IsRequestValid(maximum), "a request at the payload boundary must remain valid");

    auto oversized = maximum;
    oversized.Payload.assign(apc::control::c_maxPayloadBytes / sizeof(wchar_t) + 1, L'x');
    Check(apc::control::client::SendRequest(transport, oversized, response) ==
                  apc::control::client::SendResult::InvalidRequest &&
              transport.Attempts == 0,
          "oversized requests must be rejected before opening a pipe");

    transport.Steps.push_back({apc::control::client::AttemptResult::NotConnected, {}});
    Check(apc::control::client::SendRequest(transport, Request(apc::control::CommandType::Status), response) ==
                  apc::control::client::SendResult::Unavailable &&
              transport.Launches == 0,
          "status must never launch the tray app");
}

void TestLaunchAndComplete() {
    FakeTransport transport;
    auto server = std::make_shared<Identity>();
    transport.Steps.push_back({apc::control::client::AttemptResult::NotConnected, {}});
    transport.Steps.push_back({apc::control::client::AttemptResult::Complete, server});
    apc::control::Response response;
    Check(apc::control::client::SendRequest(transport, Request(), response) ==
                  apc::control::client::SendResult::Complete &&
              transport.Attempts == 2 && transport.Launches == 1,
          "a command may launch the app and complete against the newly observed server");
}

void TestIndeterminateReplaysOnlyToSameServer() {
    FakeTransport transport;
    auto firstServer = std::make_shared<Identity>();
    auto differentServer = std::make_shared<Identity>();
    transport.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, firstServer});
    transport.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, firstServer});
    transport.Steps.push_back({apc::control::client::AttemptResult::Complete, firstServer});
    apc::control::Response response;
    Check(apc::control::client::SendRequest(transport, Request(), response) ==
                  apc::control::client::SendResult::Complete &&
              transport.Attempts == 3 && !transport.SawUnexpectedReplayTarget,
          "an indeterminate command must reuse its correlation ID only against the same server");

    FakeTransport changed;
    changed.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, firstServer});
    changed.Steps.push_back({apc::control::client::AttemptResult::Complete, differentServer});
    Check(apc::control::client::SendRequest(changed, Request(), response) ==
                  apc::control::client::SendResult::Indeterminate &&
              changed.SawUnexpectedReplayTarget && changed.Launches == 0,
          "an indeterminate command must never be replayed to a different process identity");
}

void TestRejectedEndpointMayLaunchTrustedApp() {
    FakeTransport transport;
    transport.Steps.push_back({apc::control::client::AttemptResult::Rejected, std::make_shared<Identity>()});
    transport.Steps.push_back({apc::control::client::AttemptResult::Complete, std::make_shared<Identity>()});
    apc::control::Response response;
    Check(apc::control::client::SendRequest(transport, Request(), response) ==
                  apc::control::client::SendResult::Complete &&
              transport.Attempts == 2 && transport.Launches == 1,
          "a rejected endpoint may trigger the trusted packaged app but must never receive the request");
}

void TestOverallDeadlineBoundsReplay() {
    FakeTransport transport;
    auto server = std::make_shared<Identity>();
    transport.Steps.push_back(
        {apc::control::client::AttemptResult::Indeterminate, server, apc::control::ExitCode::Success, true});
    transport.Steps.push_back({apc::control::client::AttemptResult::Complete, server});
    apc::control::Response response;
    Check(apc::control::client::SendRequest(transport, Request(), response, 250, 10000, 1) ==
                  apc::control::client::SendResult::Indeterminate &&
              transport.Attempts == 1,
          "the shared end-to-end deadline must prevent late replay attempts");

    FakeTransport sharedDeadline;
    sharedDeadline.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, server});
    sharedDeadline.Steps.push_back({apc::control::client::AttemptResult::Complete, server});
    Check(apc::control::client::SendRequest(sharedDeadline, Request(), response) ==
                  apc::control::client::SendResult::Complete &&
              sharedDeadline.Deadlines.size() == 2 &&
              sharedDeadline.Deadlines.front() == sharedDeadline.Deadlines.back(),
          "every initial and replay attempt must share one absolute deadline");
}

void TestLaunchAndReplayBoundaries() {
    apc::control::Response response;
    auto server = std::make_shared<Identity>();

    FakeTransport launchFailure;
    launchFailure.LaunchSucceeds = false;
    launchFailure.Steps.push_back({apc::control::client::AttemptResult::NotConnected, {}});
    Check(apc::control::client::SendRequest(launchFailure, Request(), response) ==
                  apc::control::client::SendResult::Unavailable &&
              launchFailure.Attempts == 1 && launchFailure.Launches == 1,
          "a failed package launch must not produce a second transport attempt");

    FakeTransport launchedReplay;
    launchedReplay.Steps.push_back({apc::control::client::AttemptResult::NotConnected, {}});
    launchedReplay.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, server});
    launchedReplay.Steps.push_back({apc::control::client::AttemptResult::Complete, server});
    Check(apc::control::client::SendRequest(launchedReplay, Request(), response) ==
                  apc::control::client::SendResult::Complete &&
              launchedReplay.Attempts == 3 && launchedReplay.Launches == 1,
          "an indeterminate launched attempt must replay only against that launched server");

    FakeTransport missingIdentity;
    missingIdentity.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, {}});
    Check(apc::control::client::SendRequest(missingIdentity, Request(), response) ==
                  apc::control::client::SendResult::Indeterminate &&
              missingIdentity.Attempts == 1 && missingIdentity.Launches == 0,
          "an indeterminate attempt without server identity must never replay or launch");

    FakeTransport exhausted;
    exhausted.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, server});
    exhausted.Steps.push_back({apc::control::client::AttemptResult::NotConnected, server});
    exhausted.Steps.push_back({apc::control::client::AttemptResult::NotConnected, server});
    Check(apc::control::client::SendRequest(exhausted, Request(), response) ==
                  apc::control::client::SendResult::Indeterminate &&
              exhausted.Attempts == 3 && exhausted.Launches == 0,
          "exhausted same-server replay must stay indeterminate and must not launch");
    Check(exhausted.Correlations.size() == 3 && exhausted.Correlations[0] == exhausted.Correlations[1] &&
              exhausted.Correlations[1] == exhausted.Correlations[2] && exhausted.Waits[0] == 250 &&
              exhausted.Waits[1] == 2000 && exhausted.Waits[2] == 2000,
          "all replay attempts must preserve correlation and use bounded connection waits");

    FakeTransport statusReplay;
    statusReplay.Steps.push_back({apc::control::client::AttemptResult::Indeterminate, server});
    statusReplay.Steps.push_back({apc::control::client::AttemptResult::Complete, server});
    Check(apc::control::client::SendRequest(statusReplay, Request(apc::control::CommandType::Status), response) ==
                  apc::control::client::SendResult::Complete &&
              statusReplay.Attempts == 2 && statusReplay.Launches == 0,
          "status may confirm an indeterminate same-server request but must never launch the app");

    FakeTransport expiredBeforeLaunch;
    expiredBeforeLaunch.Steps.push_back(
        {apc::control::client::AttemptResult::NotConnected, {}, apc::control::ExitCode::Success, true});
    Check(apc::control::client::SendRequest(expiredBeforeLaunch, Request(), response, 250, 10000, 1) ==
                  apc::control::client::SendResult::Unavailable &&
              expiredBeforeLaunch.Attempts == 1 && expiredBeforeLaunch.Launches == 0,
          "an expired overall deadline must suppress package launch");
}
} // namespace

int RunCommandClientTests() {
    TestInvalidAndStatusNeverLaunch();
    TestLaunchAndComplete();
    TestIndeterminateReplaysOnlyToSameServer();
    TestRejectedEndpointMayLaunchTrustedApp();
    TestOverallDeadlineBoundsReplay();
    TestLaunchAndReplayBoundaries();
    return g_failures;
}
