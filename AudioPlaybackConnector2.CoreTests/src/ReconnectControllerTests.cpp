#include <core/ReconnectController.hpp>
#include <control/CommandProtocol.hpp>
#include <services/ToastXmlSanitizer.hpp>

#include <array>
#include <atomic>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestFullBackoffSequence() {
    constexpr std::wstring_view id = L"device-a";
    constexpr std::array expectedDelays{5, 10, 20, 40, 60, 60, 60, 60, 60, 60};
    ReconnectController controller;

    for (std::size_t index = 0; index < expectedDelays.size(); ++index) {
        auto decision = controller.PrepareSchedule(id, false);
        Check(decision.ShouldSchedule, "each incomplete attempt must schedule");
        Check(decision.Attempt == index + 1, "attempt number must advance only after a completed failure");
        Check(decision.Delay == std::chrono::seconds(expectedDelays[index]), "backoff delay must be deterministic");
        Check(controller.HasPendingTimer(id), "scheduled timer must be reported as pending");
        Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "a device may have only one pending timer");
        Check(controller.ClaimTimer(decision.Token), "the current timer token must be claimable exactly once");
        Check(!controller.ClaimTimer(decision.Token), "a claimed timer token must not be claimable twice");

        auto completion = controller.CompleteAttemptFailed(decision.Token);
        Check(completion.AttemptCompleted, "a claimed attempt failure must be accepted");
        Check(completion.NotifyFailed == (index + 1 == expectedDelays.size()),
              "terminal failure must be notified exactly on attempt ten");
        Check(controller.Attempts(id) == index + 1, "only completed attempts count toward the limit");
    }

    auto terminal = controller.PrepareSchedule(id, false);
    Check(!terminal.ShouldSchedule, "no timer may be scheduled after the terminal attempt");
    Check(!terminal.NotifyFailed, "terminal failure notification must be one-shot");
}

void TestSuccessAndStaleTokens() {
    constexpr std::wstring_view id = L"device-b";
    ReconnectController controller;

    auto first = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(first.Token), "first timer must be claimable");
    auto firstFailure = controller.CompleteAttemptFailed(first.Token);
    Check(firstFailure.AttemptCompleted, "first failure must complete");

    auto second = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(second.Token), "second timer must be claimable");
    controller.CompleteAttemptSucceeded(second.Token);
    Check(controller.Attempts(id) == 0, "success must reset attempts");
    Check(!controller.HasPendingTimer(id), "success must clear busy reconnect state");
    Check(!controller.CompleteAttemptFailed(second.Token).AttemptCompleted,
          "a success-invalidated token must not mutate state later");

    auto stale = controller.PrepareSchedule(id, false);
    controller.BeginManualOperation(id);
    Check(!controller.ClaimTimer(stale.Token), "manual operations must invalidate older automatic timers");
}

void TestCancellationAndTimerCreationFailure() {
    constexpr std::wstring_view id = L"device-c";
    ReconnectController controller;

    auto pending = controller.PrepareSchedule(id, false);
    controller.CancelDevice(id);
    Check(!controller.ClaimTimer(pending.Token), "device cancellation must invalidate a pending timer");
    Check(controller.IsCancelled(id), "device cancellation must remain observable");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "cancelled devices must not silently restart");

    controller.BeginManualOperation(id);
    auto retryable = controller.PrepareSchedule(id, false);
    controller.HandleTimerCreateFailed(retryable.Token);
    auto replacement = controller.PrepareSchedule(id, false);
    Check(replacement.ShouldSchedule, "timer creation failure must release the pending slot");
    Check(replacement.Attempt == 1, "timer creation failure must not consume an attempt");

    controller.CancelPendingReconnects();
    Check(!controller.ClaimTimer(replacement.Token), "global cancellation must invalidate every timer token");
    Check(controller.AllReconnectsCancelled(), "global cancellation must remain observable");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "global cancellation must block new timers");
}

void TestObservedConnectionInvalidatesAttempt() {
    constexpr std::wstring_view id = L"device-d";
    ReconnectController controller;

    auto decision = controller.PrepareSchedule(id, false);
    Check(controller.ClaimTimer(decision.Token), "timer must be claimable before observed success");
    controller.CompleteConnectionSucceeded(id);
    Check(!controller.CompleteAttemptFailed(decision.Token).AttemptCompleted,
          "an observed connection must invalidate a late failure completion");
    Check(controller.Attempts(id) == 0, "an observed connection must reset attempts");
}

void TestBlockedTimerDoesNotRemainPending() {
    constexpr std::wstring_view id = L"device-blocked";
    ReconnectController controller;

    auto blocked = controller.PrepareSchedule(id, false);
    Check(controller.RetireTimer(blocked.Token), "a blocked current timer must be retired");
    Check(!controller.HasPendingTimer(id), "a fired blocked timer must release its pending slot");
    auto replacement = controller.PrepareSchedule(id, false);
    Check(replacement.ShouldSchedule, "a later connection loss must be able to schedule after a blocked timer");
    Check(replacement.Attempt == blocked.Attempt, "retiring a blocked timer must not consume an attempt");
    Check(replacement.Token.DeviceGeneration != blocked.Token.DeviceGeneration,
          "a replacement timer must not reuse the retired token generation");
    Check(!controller.ClaimTimer(blocked.Token), "a retired callback must not claim its replacement timer");
    Check(controller.ClaimTimer(replacement.Token), "the replacement timer must remain claimable");
}

void TestBusyTimerDeferralPreservesReconnect() {
    constexpr std::wstring_view id = L"device-deferred";
    ReconnectController controller;

    auto original = controller.PrepareSchedule(id, false);
    auto deferred = controller.DeferTimer(original.Token);
    Check(deferred.ShouldSchedule, "a current timer blocked by another operation must be deferred");
    Check(deferred.Attempt == original.Attempt, "deferral must not consume a reconnect attempt");
    Check(deferred.Token.DeviceGeneration != original.Token.DeviceGeneration,
          "deferral must invalidate the original callback token");
    Check(controller.HasPendingTimer(id), "a deferred reconnect must remain observably pending");
    Check(!controller.ClaimTimer(original.Token), "the original callback must be stale after deferral");
    Check(controller.ClaimTimer(deferred.Token), "the deferred callback must remain claimable");

    auto next = controller.PrepareSchedule(L"device-deferred-stale", false);
    auto replacement = controller.DeferTimer(next.Token);
    Check(!controller.DeferTimer(next.Token).ShouldSchedule,
          "a stale callback must not replace a newer deferred timer");
    Check(controller.ClaimTimer(replacement.Token), "stale deferral must leave the newer timer intact");
}

void TestReconnectPolicyDoesNotBecomeUserCancellation() {
    constexpr std::wstring_view id = L"device-policy";
    ReconnectController controller;

    auto pending = controller.PrepareSchedule(id, false);
    controller.SetPolicyEnabled(id, false);
    Check(!controller.ClaimTimer(pending.Token), "disabling policy must invalidate a pending timer");
    Check(!controller.IsCancelled(id), "policy disable must not be recorded as a user cancellation");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule, "disabled policy must block reconnect scheduling");

    controller.SetPolicyEnabled(id, true);
    Check(controller.PrepareSchedule(id, false).ShouldSchedule,
          "re-enabling policy must permit a later connection-loss reconnect");
}

void TestReconnectPolicyDoesNotClearUserCancellation() {
    constexpr std::wstring_view id = L"device-user-cancelled";
    ReconnectController controller;

    controller.CancelDevice(id);
    controller.SetPolicyEnabled(id, false);
    controller.SetPolicyEnabled(id, true);
    Check(controller.IsCancelled(id), "policy changes must not erase an explicit user cancellation");
    Check(!controller.PrepareSchedule(id, false).ShouldSchedule,
          "a user-cancelled device must remain blocked until a manual operation");

    controller.BeginManualOperation(id);
    Check(controller.PrepareSchedule(id, false).ShouldSchedule,
          "a later manual operation must explicitly clear user cancellation");
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : m_value(value) {}
    ~UniqueHandle() {
        if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value);
    }
    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            UniqueHandle cleanup(std::exchange(m_value, std::exchange(other.m_value, nullptr)));
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_value; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_value = nullptr;
};

bool CreatePipePair(UniqueHandle& server, UniqueHandle& client) {
    static std::atomic_uint32_t counter = 0;
    const auto name = L"\\\\.\\pipe\\AudioPlaybackConnector2.CoreTests." + std::to_wstring(GetCurrentProcessId()) +
                      L"." + std::to_wstring(++counter);
    server = UniqueHandle(CreateNamedPipeW(name.c_str(),
                                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                           1,
                                           apc::control::c_pipeBufferBytes,
                                           apc::control::c_pipeBufferBytes,
                                           0,
                                           nullptr));
    if (!server) return false;

    UniqueHandle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = connected.get();
    if (ConnectNamedPipe(server.get(), &overlapped)) return false;
    if (GetLastError() != ERROR_IO_PENDING) return false;

    client = UniqueHandle(CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!client) return false;
    if (WaitForSingleObject(connected.get(), 1000) != WAIT_OBJECT_0) return false;

    DWORD transferred = 0;
    return GetOverlappedResult(server.get(), &overlapped, &transferred, FALSE) != FALSE;
}

void TestCommandProtocolRoundTrip() {
    UniqueHandle server;
    UniqueHandle client;
    Check(CreatePipePair(server, client), "overlapped test pipe must connect");
    if (!server || !client) return;

    apc::control::Request sentRequest;
    sentRequest.Command = apc::control::CommandType::Connect;
    sentRequest.Target = apc::control::TargetKind::Id;
    sentRequest.Flags = apc::control::CommandFlagJson;
    sentRequest.Payload = L"device-id";
    sentRequest.CorrelationId = {0x1111222233334444, 0x5555666677778888};
    auto deadline = apc::control::DeadlineAfter(1000);
    Check(apc::control::WriteRequest(client.get(), sentRequest, nullptr, deadline) == apc::control::IoStatus::Success,
          "request write must complete");

    apc::control::Request receivedRequest;
    Check(apc::control::ReadRequest(server.get(), receivedRequest, nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "request read must complete");
    Check(receivedRequest.Command == sentRequest.Command && receivedRequest.Target == sentRequest.Target &&
              receivedRequest.Flags == sentRequest.Flags && receivedRequest.Payload == sentRequest.Payload &&
              receivedRequest.CorrelationId == sentRequest.CorrelationId,
          "request fields must survive a pipe round trip");

    apc::control::Response sentResponse{apc::control::ExitCode::Success, L"response", sentRequest.CorrelationId};
    Check(apc::control::WriteResponse(server.get(), sentResponse, nullptr, deadline) == apc::control::IoStatus::Success,
          "response write must complete");
    apc::control::Response receivedResponse;
    Check(apc::control::ReadResponse(client.get(), receivedResponse, nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "response read must complete");
    Check(receivedResponse.Code == sentResponse.Code && receivedResponse.Payload == sentResponse.Payload &&
              receivedResponse.CorrelationId == sentResponse.CorrelationId,
          "response fields must survive a pipe round trip");

    Check(apc::control::WriteAcknowledgement(client.get(), sentRequest.CorrelationId, nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "response acknowledgement must be written");
    Check(apc::control::ReadAcknowledgement(server.get(), sentRequest.CorrelationId, nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "response acknowledgement must preserve correlation");
}

void TestCommandProtocolDelayedResponseReader() {
    UniqueHandle server;
    UniqueHandle client;
    Check(CreatePipePair(server, client), "delayed-reader pipe must connect");
    if (!server || !client) return;

    constexpr apc::control::CorrelationId correlation{0xABCDEF0123456789, 0x1020304050607080};
    apc::control::Response sent{apc::control::ExitCode::Success, L"delayed-response", correlation};
    std::atomic writeSucceeded = false;
    std::atomic acknowledgementReceived = false;
    std::jthread serverFlow([&] {
        const auto deadline = apc::control::DeadlineAfter(2000);
        writeSucceeded =
            apc::control::WriteResponse(server.get(), sent, nullptr, deadline) == apc::control::IoStatus::Success;
        acknowledgementReceived = apc::control::ReadAcknowledgement(server.get(), correlation, nullptr, deadline) ==
                                  apc::control::IoStatus::Success;
        DisconnectNamedPipe(server.get());
    });

    Sleep(100);
    apc::control::Response received;
    const auto deadline = apc::control::DeadlineAfter(2000);
    Check(apc::control::ReadResponse(client.get(), received, nullptr, deadline) == apc::control::IoStatus::Success,
          "server must retain a completed response until a delayed client reads it");
    Check(received.CorrelationId == correlation && received.Payload == sent.Payload,
          "delayed response must remain intact");
    Check(apc::control::WriteAcknowledgement(client.get(), correlation, nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "delayed client must acknowledge the complete response");
    serverFlow.join();
    Check(writeSucceeded.load() && acknowledgementReceived.load(),
          "server must observe delivery acknowledgement before disconnecting");
}

void TestCommandProtocolStrictValidation() {
    apc::control::Request request;
    request.Command = apc::control::CommandType::ToggleLast;
    request.Target = apc::control::TargetKind::Default;
    request.CorrelationId = {1, 2};
    Check(apc::control::IsRequestValid(request), "default-target toggle must remain valid");

    request.Target = apc::control::TargetKind::Id;
    request.Payload = L"device-id";
    Check(apc::control::IsRequestValid(request), "explicit-target toggle must remain valid");

    request.Flags = 0x80000000;
    Check(!apc::control::IsRequestValid(request), "unknown request flags must be rejected");
    request.Flags = apc::control::CommandFlagNone;
    request.Payload.push_back(L'\0');
    Check(!apc::control::IsRequestValid(request), "embedded NUL payloads must be rejected");

    request = {};
    request.Command = apc::control::CommandType::Show;
    request.CorrelationId = {3, 4};
    Check(apc::control::IsRequestValid(request), "payload-free show command must be valid");
    request.Payload = L"unexpected";
    Check(!apc::control::IsRequestValid(request), "show payloads must be rejected");
}

void TestCommandProtocolTimeoutAndCancellation() {
    {
        UniqueHandle server;
        UniqueHandle client;
        Check(CreatePipePair(server, client), "timeout test pipe must connect");
        apc::control::Request request;
        const auto started = GetTickCount64();
        const auto status = apc::control::ReadRequest(server.get(), request, nullptr, apc::control::DeadlineAfter(50));
        Check(status == apc::control::IoStatus::Timeout, "silent clients must hit the absolute read deadline");
        Check(GetTickCount64() - started < 1000, "read timeout must remain bounded");
    }

    {
        UniqueHandle server;
        UniqueHandle client;
        Check(CreatePipePair(server, client), "cancellation test pipe must connect");
        UniqueHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::jthread cancel([event = stopEvent.get()] {
            Sleep(20);
            SetEvent(event);
        });
        apc::control::Request request;
        const auto started = GetTickCount64();
        const auto status =
            apc::control::ReadRequest(server.get(), request, stopEvent.get(), apc::control::DeadlineAfter(2000));
        Check(status == apc::control::IoStatus::Cancelled, "stop events must cancel pending overlapped reads");
        Check(GetTickCount64() - started < 1000, "read cancellation must remain bounded");
    }
}

void TestCommandProtocolRejectsInvalidHeader() {
    UniqueHandle server;
    UniqueHandle client;
    Check(CreatePipePair(server, client), "invalid-header test pipe must connect");
    if (!server || !client) return;

    apc::control::RequestHeader invalidHeader;
    invalidHeader.CorrelationHigh = 1;
    invalidHeader.CorrelationLow = 2;
    invalidHeader.Command = static_cast<uint32_t>(apc::control::CommandType::Show);
    invalidHeader.Target = static_cast<uint32_t>(apc::control::TargetKind::None);
    invalidHeader.PayloadBytes = apc::control::c_maxPayloadBytes + sizeof(wchar_t);
    const auto deadline = apc::control::DeadlineAfter(1000);
    Check(apc::control::WriteExact(client.get(), &invalidHeader, sizeof(invalidHeader), nullptr, deadline) ==
              apc::control::IoStatus::Success,
          "invalid test header must be written");
    apc::control::Request request;
    Check(apc::control::ReadRequest(server.get(), request, nullptr, deadline) == apc::control::IoStatus::InvalidData,
          "oversized payload headers must be rejected before allocation");
}

void TestToastXmlSanitization() {
    Check(apc::toast::EscapeXml(L"&<>\"'\t\n\r") == L"&amp;&lt;&gt;&quot;&apos;\t\n\r",
          "toast XML metacharacters must be escaped without removing legal whitespace");

    std::wstring invalid{L'A',
                         L'\0',
                         static_cast<wchar_t>(0x1F),
                         static_cast<wchar_t>(0xD800),
                         L'B',
                         static_cast<wchar_t>(0xDC00),
                         L'C',
                         static_cast<wchar_t>(0xFFFE)};
    Check(apc::toast::EscapeXml(invalid) == L"A\uFFFD\uFFFD\uFFFDB\uFFFDC\uFFFD",
          "illegal XML controls and unpaired UTF-16 surrogates must be replaced");

    std::wstring surrogatePair{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
    Check(apc::toast::EscapeXml(surrogatePair) == surrogatePair, "valid UTF-16 surrogate pairs must be preserved");
}

} // namespace

int RunAdaptiveResourcePolicyTests();
int RunAppWorkCoordinatorTests();
int RunAutoReconnectPlannerTests();
int RunCommandClientTests();
int RunCommandLineControlServerTests();
int RunControlUiActionGateTests();
int RunControlTargetMatcherTests();
int RunDeviceOperationCoordinatorTests();
int RunDevicePickerSnapshotTests();
int RunResourcePressureMonitorTests();
int RunRuntimeApartmentTests();
int RunSettingsLimitsTests();
int RunSingleInstanceGuardTests();
int RunTrayTooltipBuilderTests();
int RunUpdateCoordinatorTests();

int main() {
    TestFullBackoffSequence();
    TestSuccessAndStaleTokens();
    TestCancellationAndTimerCreationFailure();
    TestObservedConnectionInvalidatesAttempt();
    TestBlockedTimerDoesNotRemainPending();
    TestBusyTimerDeferralPreservesReconnect();
    TestReconnectPolicyDoesNotBecomeUserCancellation();
    TestReconnectPolicyDoesNotClearUserCancellation();
    TestCommandProtocolRoundTrip();
    TestCommandProtocolDelayedResponseReader();
    TestCommandProtocolStrictValidation();
    TestCommandProtocolTimeoutAndCancellation();
    TestCommandProtocolRejectsInvalidHeader();
    TestToastXmlSanitization();
    g_failures += RunAdaptiveResourcePolicyTests();
    g_failures += RunAppWorkCoordinatorTests();
    g_failures += RunAutoReconnectPlannerTests();
    g_failures += RunCommandClientTests();
    g_failures += RunCommandLineControlServerTests();
    g_failures += RunControlUiActionGateTests();
    g_failures += RunControlTargetMatcherTests();
    g_failures += RunDeviceOperationCoordinatorTests();
    g_failures += RunDevicePickerSnapshotTests();
    g_failures += RunResourcePressureMonitorTests();
    g_failures += RunRuntimeApartmentTests();
    g_failures += RunSettingsLimitsTests();
    g_failures += RunSingleInstanceGuardTests();
    g_failures += RunTrayTooltipBuilderTests();
    g_failures += RunUpdateCoordinatorTests();

    if (g_failures != 0) {
        std::cerr << g_failures << " core test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
