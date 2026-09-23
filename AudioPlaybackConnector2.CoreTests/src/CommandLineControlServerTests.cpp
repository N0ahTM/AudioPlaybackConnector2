#include "TestCheck.hpp"

#include <control/CommandPipeSecurity.hpp>
#include <control/CommandProtocol.hpp>
#include <control/CommandPipeIo.hpp>
#include "AppTestFixture.hpp"
#include <services/CommandLineControlServer.hpp>

#include <aclapi.h>
#include <appmodel.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

std::atomic_uint64_t g_pipeSequence = 0;

class Event {
public:
    Event() : m_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!m_handle) throw std::runtime_error("CreateEventW failed");
    }
    Event(Event const&) = delete;
    Event& operator=(Event const&) = delete;

    void Signal() const noexcept { SetEvent(m_handle.get()); }
    [[nodiscard]] bool Wait(DWORD timeoutMs) const noexcept {
        return WaitForSingleObject(m_handle.get(), timeoutMs) == WAIT_OBJECT_0;
    }

private:
    wil::unique_handle m_handle;
};

std::wstring UniquePipeName(std::wstring_view testName) {
    return L"\\\\.\\pipe\\AudioPlaybackConnector2.ServerTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(++g_pipeSequence) + L"." + std::wstring(testName);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Bounded Test Host /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool IsCurrentTestProcessClient(HANDLE pipe) noexcept {
    ULONG clientId = 0;
    return GetNamedPipeClientProcessId(pipe, &clientId) && clientId == GetCurrentProcessId() &&
           apc::control::IsTrustedPeerProcess(GetCurrentProcess(), clientId);
}

CommandLineControlServer::Options TestOptions(std::wstring_view testName, std::size_t instances = 2) {
    CommandLineControlServer::Options serverOptions;
    serverOptions.PipeName = UniquePipeName(testName);
    serverOptions.PipeInstanceCount = instances;
    serverOptions.RequestTimeoutMs = 500;
    serverOptions.HandlerTimeoutMs = 2000;
    serverOptions.ResponseTimeoutMs = 500;
    serverOptions.AcknowledgementTimeoutMs = 100;
    serverOptions.RetryDelayMs = 10;
    serverOptions.RequestRecordLifetime = 2s;
    serverOptions.RetryStartupFailures = false;
    serverOptions.IsTrustedClient = IsCurrentTestProcessClient;
    return serverOptions;
}

wil::unique_handle OpenClient(std::wstring const& targetPipeName, DWORD timeoutMs = 2000) {
    const auto deadline = GetTickCount64() + timeoutMs;
    DWORD baseError = ERROR_SUCCESS;
    while (true) {
        for (std::size_t index = 0; index < apc::control::c_pipeInstanceCount; ++index) {
            const auto instanceName = apc::control::PipeInstanceName(targetPipeName, index);
            wil::unique_handle pipeHandle(CreateFileW(instanceName.c_str(),
                                                      GENERIC_READ | FILE_WRITE_DATA,
                                                      0,
                                                      nullptr,
                                                      OPEN_EXISTING,
                                                      FILE_FLAG_OVERLAPPED,
                                                      nullptr));
            if (pipeHandle) return pipeHandle;
            if (index == 0) baseError = GetLastError();
        }
        if (GetTickCount64() >= deadline) {
            std::cerr << "OpenClient timeout, base Win32 error " << baseError << '\n';
            return {};
        }
        Sleep(5);
    }
}

apc::control::Request MakeRequest(std::uint64_t id,
                                  apc::control::CommandType command = apc::control::CommandType::Show) {
    apc::control::Request request;
    request.Command = command;
    request.CorrelationId = {0xC001C0DE00000000ull, id};
    return request;
}

bool WriteRequest(HANDLE pipeHandle, apc::control::Request const& commandRequest, DWORD timeoutMs = 1000) {
    return apc::control::WriteRequest(pipeHandle, commandRequest, nullptr, apc::control::DeadlineAfter(timeoutMs)) ==
           apc::control::IoStatus::Success;
}

std::optional<apc::control::Response>
ReadResponse(HANDLE pipe, apc::control::CorrelationId correlation, bool acknowledge = true, DWORD timeoutMs = 1000) {
    apc::control::Response response;
    const auto deadline = apc::control::DeadlineAfter(timeoutMs);
    if (apc::control::ReadResponse(pipe, response, nullptr, deadline) != apc::control::IoStatus::Success ||
        response.CorrelationId != correlation) {
        return std::nullopt;
    }
    if (acknowledge &&
        apc::control::WriteAcknowledgement(pipe, correlation, nullptr, deadline) != apc::control::IoStatus::Success) {
        return std::nullopt;
    }
    return response;
}

std::optional<apc::control::Response> Exchange(std::wstring const& pipeName,
                                               apc::control::Request const& request,
                                               bool acknowledge = true,
                                               DWORD timeoutMs = 1000) {
    auto pipe = OpenClient(pipeName, timeoutMs);
    if (!pipe || !WriteRequest(pipe.get(), request, timeoutMs)) return std::nullopt;
    return ReadResponse(pipe.get(), request.CorrelationId, acknowledge, timeoutMs);
}

bool WaitForServerDisconnect(HANDLE pipe, DWORD timeoutMs = 1000) {
    std::byte unexpectedByte{};
    return apc::control::ReadExact(pipe, &unexpectedByte, 1, nullptr, apc::control::DeadlineAfter(timeoutMs)) ==
           apc::control::IoStatus::Closed;
}

bool CompleteRoundTrip(std::wstring const& pipeName, apc::control::Request const& request) {
    auto pipe = OpenClient(pipeName);
    if (!pipe || !WriteRequest(pipe.get(), request)) return false;
    auto response = ReadResponse(pipe.get(), request.CorrelationId);
    return response && response->Code == apc::control::ExitCode::Success && WaitForServerDisconnect(pipe.get());
}

bool WaitUntil(std::function<bool()> predicate, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (predicate()) return true;
        Sleep(5);
    }
    return predicate();
}

void TestProductionRoundTripFragmentationAndRearm() {
    auto options = TestOptions(L"roundtrip", 1);
    const auto pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    std::atomic_int calls = 0;
    server.Start([&](apc::control::Request const& request, std::stop_token, std::uint64_t) {
        ++calls;
        return apc::control::Response{apc::control::ExitCode::Success, L"echo:" + request.Payload};
    });
    Check(server.IsRunning(), "production server must start synchronously");

    auto pipe = OpenClient(pipeName);
    Check(static_cast<bool>(pipe), "fragmentation client must connect");
    if (pipe) {
        auto request = MakeRequest(1, apc::control::CommandType::Connect);
        request.Target = apc::control::TargetKind::Id;
        request.Payload = L"device-id";
        apc::control::RequestHeader header;
        header.CorrelationHigh = request.CorrelationId.High;
        header.CorrelationLow = request.CorrelationId.Low;
        header.Command = static_cast<std::uint32_t>(request.Command);
        header.Target = static_cast<std::uint32_t>(request.Target);
        header.PayloadBytes = *apc::control::PayloadByteCount(request.Payload);
        auto* headerBytes = reinterpret_cast<std::byte*>(&header);
        const auto deadline = apc::control::DeadlineAfter(1000);
        Check(apc::control::WriteExact(pipe.get(), headerBytes, 7, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented header prefix must be accepted");
        Check(apc::control::WriteExact(pipe.get(), headerBytes + 7, sizeof(header) - 7, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented header suffix must be accepted");
        auto* payloadBytes = reinterpret_cast<std::byte*>(request.Payload.data());
        Check(apc::control::WriteExact(pipe.get(), payloadBytes, 3, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented payload prefix must be accepted");
        Check(apc::control::WriteExact(pipe.get(), payloadBytes + 3, header.PayloadBytes - 3, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented payload suffix must be accepted");
        auto response = ReadResponse(pipe.get(), request.CorrelationId);
        Check(response && response->Payload == L"echo:device-id", "fragmented request must reach the real handler");
    }
    pipe.reset();

    auto second = Exchange(pipeName, MakeRequest(2));
    Check(second && second->Code == apc::control::ExitCode::Success,
          "pipe instance must rearm after a complete exchange");
    Check(calls.load() == 2, "two distinct production requests must execute exactly twice");
    server.Stop();
}

void TestDisconnectBeforeResponseRetriesExactlyOnce() {
    auto options = TestOptions(L"disconnect-before", 2);
    const auto pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    Event started;
    Event release;
    Event finished;
    std::atomic_int calls = 0;
    server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
        ++calls;
        started.Signal();
        (void)release.Wait(1500);
        finished.Signal();
        return apc::control::Response{apc::control::ExitCode::Success, L"once"};
    });

    auto request = MakeRequest(10);
    auto abandoned = OpenClient(pipeName);
    Check(abandoned && WriteRequest(abandoned.get(), request), "abandoning client must send its complete request");
    Check(started.Wait(1000), "handler must start before the first client disconnects");
    abandoned.reset();
    release.Signal();
    Check(finished.Wait(1000), "abandoned request handler must finish");

    auto retry = Exchange(pipeName, request);
    Check(retry && retry->Payload == L"once", "retry must receive the cached response after an early disconnect");
    Check(calls.load() == 1, "disconnect before response must not execute the handler twice");
    server.Stop();
}

void TestDisconnectAfterResponseBeforeAckRetriesExactlyOnce() {
    auto options = TestOptions(L"disconnect-before-ack", 2);
    const auto pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    std::atomic_int calls = 0;
    server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
        ++calls;
        return apc::control::Response{apc::control::ExitCode::Success, L"cached"};
    });

    auto request = MakeRequest(20);
    auto first = Exchange(pipeName, request, false);
    Check(first && first->Payload == L"cached", "first client must read the response before dropping its ACK");
    auto retry = Exchange(pipeName, request);
    Check(retry && retry->Payload == L"cached", "missing ACK must leave the response replayable");
    Check(calls.load() == 1, "disconnect after response but before ACK must not execute twice");
    server.Stop();
}

void TestParallelDuplicatesAndCorrelationConflict() {
    {
        auto options = TestOptions(L"parallel", 2);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        Event started;
        Event release;
        std::atomic_int calls = 0;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++calls;
            started.Signal();
            (void)release.Wait(1500);
            return apc::control::Response{apc::control::ExitCode::Success, L"shared"};
        });

        auto request = MakeRequest(30);
        auto first = OpenClient(pipeName);
        auto second = OpenClient(pipeName);
        Check(first && second && WriteRequest(first.get(), request) && WriteRequest(second.get(), request),
              "parallel duplicate clients must send the same request");
        Check(started.Wait(1000), "one parallel handler must start");
        release.Signal();
        auto firstResponse = ReadResponse(first.get(), request.CorrelationId);
        auto secondResponse = ReadResponse(second.get(), request.CorrelationId);
        Check(firstResponse && secondResponse && firstResponse->Payload == secondResponse->Payload,
              "parallel duplicates must receive the same response");
        Check(calls.load() == 1, "parallel duplicate requests must execute once");
        server.Stop();
    }

    {
        auto options = TestOptions(L"parallel-cache-pressure", 2);
        options.MaxRequestRecords = 1;
        options.AcknowledgedRecordLifetime = 1ms;
        options.ResponseTimeoutMs = 5000;
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        std::atomic_int calls = 0;
        const std::wstring payload(apc::control::c_maxPayloadBytes / sizeof(wchar_t), L'r');
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++calls;
            return apc::control::Response{apc::control::ExitCode::Success, payload};
        });

        const auto request = MakeRequest(32);
        auto first = OpenClient(pipeName);
        auto second = OpenClient(pipeName);
        Check(first && second && WriteRequest(first.get(), request) && WriteRequest(second.get(), request),
              "cache-pressure duplicates must both be accepted");

        // Reading only the header proves that the second delivery owns the
        // record. Its 64 KiB body cannot fit in the 4 KiB pipe buffer.
        apc::control::ResponseHeader secondHeader;
        auto const headerStatus = apc::control::ReadExact(
            second.get(), &secondHeader, sizeof(secondHeader), nullptr, apc::control::DeadlineAfter(1000));
        Check(headerStatus == apc::control::IoStatus::Success &&
                  secondHeader.PayloadBytes == apc::control::c_maxPayloadBytes,
              "the duplicate must begin its full response before cache pressure");
        auto firstResponse = ReadResponse(first.get(), request.CorrelationId);
        Check(firstResponse && firstResponse->Payload == payload && WaitForServerDisconnect(first.get()),
              "one duplicate must complete its response and acknowledgement");

        auto pressure = Exchange(pipeName, MakeRequest(33), true, 1000);
        Check(pressure && pressure->Code == apc::control::ExitCode::Busy,
              "cache pressure must not evict a record whose duplicate delivery is blocked on pipe I/O");

        std::wstring secondPayload(payload.size(), L'\0');
        auto const bodyStatus = apc::control::ReadExact(second.get(),
                                                        secondPayload.data(),
                                                        apc::control::c_maxPayloadBytes,
                                                        nullptr,
                                                        apc::control::DeadlineAfter(1000));
        Check(bodyStatus == apc::control::IoStatus::Success && secondPayload == payload,
              "the blocked duplicate must receive the original response after cache pressure");
        Check(apc::control::WriteAcknowledgement(
                  second.get(), request.CorrelationId, nullptr, apc::control::DeadlineAfter(1000)) ==
                      apc::control::IoStatus::Success &&
                  WaitForServerDisconnect(second.get()),
              "the retained duplicate delivery must finish normally");
        Check(calls.load() == 1, "cache pressure must not execute the duplicate again");
        auto nextResponse = Exchange(pipeName, MakeRequest(34));
        Check(nextResponse && nextResponse->Code == apc::control::ExitCode::Success &&
                  nextResponse->Payload == payload && calls.load() == 2,
              "finishing both deliveries must release cache capacity for the next command");
        server.Stop();
    }

    {
        auto options = TestOptions(L"collision", 2);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        Event started;
        Event release;
        std::atomic_int calls = 0;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++calls;
            started.Signal();
            (void)release.Wait(1500);
            return apc::control::Response{apc::control::ExitCode::Success, L"canonical"};
        });

        auto original = MakeRequest(31, apc::control::CommandType::Show);
        auto conflicting = MakeRequest(31, apc::control::CommandType::Settings);
        auto originalPipe = OpenClient(pipeName);
        Check(originalPipe && WriteRequest(originalPipe.get(), original), "canonical request must be sent");
        Check(started.Wait(1000), "canonical handler must be in flight");
        auto conflict = Exchange(pipeName, conflicting);
        Check(conflict && conflict->Code == apc::control::ExitCode::InvalidRequest,
              "same correlation with a different request must be rejected");
        release.Signal();
        auto originalResponse = ReadResponse(originalPipe.get(), original.CorrelationId, false);
        Check(originalResponse && originalResponse->Payload == L"canonical",
              "canonical response must survive conflict");
        originalPipe.reset();
        auto retry = Exchange(pipeName, original);
        Check(retry && retry->Payload == L"canonical", "conflict ACK must not evict the canonical record");
        Check(calls.load() == 1, "correlation conflict must never execute or re-enable the original request");
        server.Stop();
    }
}

void TestMalformedTimeoutOversizeAndRecovery() {
    auto options = TestOptions(L"malformed", 2);
    options.RequestTimeoutMs = 60;
    const auto pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    std::atomic_int calls = 0;
    server.Start([&](apc::control::Request const& request, std::stop_token, std::uint64_t) {
        ++calls;
        if (request.CorrelationId.Low == 42) {
            return apc::control::Response{apc::control::ExitCode::Success,
                                          std::wstring(apc::control::c_maxPayloadBytes / sizeof(wchar_t) + 1, L'x')};
        }
        if (request.CorrelationId.Low == 44) {
            return apc::control::Response{apc::control::ExitCode::Success,
                                          std::wstring(apc::control::c_maxPayloadBytes / sizeof(wchar_t), L'y')};
        }
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    });

    auto malformed = OpenClient(pipeName);
    apc::control::RequestHeader invalidHeader;
    invalidHeader.CorrelationHigh = 1;
    invalidHeader.CorrelationLow = 1;
    invalidHeader.Command = static_cast<std::uint32_t>(apc::control::CommandType::Show);
    invalidHeader.PayloadBytes = apc::control::c_maxPayloadBytes + sizeof(wchar_t);
    Check(malformed &&
              apc::control::WriteExact(
                  malformed.get(), &invalidHeader, sizeof(invalidHeader), nullptr, apc::control::DeadlineAfter(500)) ==
                  apc::control::IoStatus::Success,
          "malformed header must reach the server");
    malformed.reset();

    auto silent = OpenClient(pipeName);
    auto validHeader = apc::control::RequestHeader{};
    validHeader.CorrelationHigh = 2;
    validHeader.CorrelationLow = 2;
    validHeader.Command = static_cast<std::uint32_t>(apc::control::CommandType::Show);
    Check(silent &&
              apc::control::WriteExact(
                  silent.get(), &validHeader, sizeof(validHeader) / 2, nullptr, apc::control::DeadlineAfter(500)) ==
                  apc::control::IoStatus::Success,
          "partial header must be accepted before timeout");
    Sleep(120);
    std::byte byte{};
    const auto closed = apc::control::ReadExact(silent.get(), &byte, 1, nullptr, apc::control::DeadlineAfter(500));
    Check(closed == apc::control::IoStatus::Closed || closed == apc::control::IoStatus::Cancelled ||
              closed == apc::control::IoStatus::Failed,
          "partial client must be disconnected at the absolute request deadline");
    silent.reset();

    auto oversized = Exchange(pipeName, MakeRequest(42));
    Check(oversized && oversized->Code == apc::control::ExitCode::Indeterminate && oversized->Payload.empty(),
          "oversized handler response after a possible side effect must remain indeterminate");
    auto recovered = Exchange(pipeName, MakeRequest(43));
    Check(recovered && recovered->Payload == L"ok", "malformed and oversized requests must not poison rearm");
    auto maximum = Exchange(pipeName, MakeRequest(44), true, 2000);
    Check(maximum && maximum->Code == apc::control::ExitCode::Success &&
              maximum->Payload.size() == apc::control::c_maxPayloadBytes / sizeof(wchar_t),
          "maximum-size responses must survive 4 KiB pipe backpressure and partial writes");
    Check(calls.load() == 3, "invalid clients must never reach the handler");
    server.Stop();
}

void TestRequestStopCancelsControllerWorkBeforeDrain() {
    auto options = TestOptions(L"request-stop-controller-drain", 1);
    auto const pipeName = options.PipeName;
    apc::tests::AppFixture fixture;
    Event operationEntered;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::atomic_bool cancelled = false;
    fixture.Presentation->SettingsAction = [&](apc::app::AppCommandContext const& context) {
        operationEntered.Signal();
        std::unique_lock lock(mutex);
        changed.wait_until(
            lock, context.StopToken, std::chrono::steady_clock::now() + std::chrono::seconds(3), [] { return false; });
        cancelled = context.IsCancellationRequested();
        return apc::app::AppUiActionResult{
            cancelled ? apc::app::AppActionStatus::Cancelled : apc::app::AppActionStatus::TimedOut, std::nullopt};
    };
    CommandLineControlServer server(std::move(options));
    server.Start([&](apc::control::Request const&, std::stop_token token, std::uint64_t) {
        apc::app::AppCommandContext context;
        context.StopToken = token;
        auto result = fixture.Controller.ShowSettings(context);
        return apc::control::Response{result.Code == apc::app::AppResultCode::Cancelled
                                          ? apc::control::ExitCode::Indeterminate
                                          : apc::control::ExitCode::OperationFailed,
                                      L""};
    });
    auto client = OpenClient(pipeName);
    auto request = MakeRequest(61, apc::control::CommandType::Settings);
    Check(client && WriteRequest(client.get(), request), "request must reach the concrete controller");
    Check(operationEntered.Wait(1000), "presentation must be admitted before teardown");
    Event teardownReturned;
    std::jthread teardown([&] {
        server.RequestStop();
        fixture.Controller.Shutdown();
        teardownReturned.Signal();
    });
    Check(teardownReturned.Wait(1000),
          "request-stop cancellation must release the active controller call before drain");
    teardown.join();
    Check(cancelled && !server.IsRunning(), "teardown must cancel the admitted operation and close pipe admission");
    Check(fixture.Controller.ShowSettings().Code == apc::app::AppResultCode::Unavailable &&
              fixture.Presentation->SettingsCalls == 1,
          "after controller drain no late presentation call may run");
    client.reset();
    server.Stop();
}

void TestStopLifecycleAndRearmRetry() {
    {
        auto options = TestOptions(L"start-stop-race", 1);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        auto handler = [](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"after-race"};
        };
        for (int iteration = 0; iteration < 12; ++iteration) {
            Event begin;
            std::jthread starter([&] {
                (void)begin.Wait(INFINITE);
                server.Start(handler);
            });
            std::jthread stopper([&] {
                (void)begin.Wait(INFINITE);
                server.Stop();
            });
            begin.Signal();
            starter.join();
            stopper.join();
            server.Stop();
            Check(!server.IsRunning(), "concurrent startup and stop must leave a stoppable server");
            server.Start(handler);
            auto response = Exchange(pipeName, MakeRequest(490 + iteration));
            Check(response && response->Payload == L"after-race",
                  "concurrent startup and stop must preserve a working restart");
            server.Stop();
        }
    }

    {
        auto options = TestOptions(L"stop-silent", 1);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
        });
        auto silent = OpenClient(pipeName);
        const auto started = GetTickCount64();
        server.Stop();
        Check(GetTickCount64() - started < 1500, "Stop must cancel and drain a silent overlapped client");
        Check(!server.IsRunning(), "Stop must publish the stopped state");
        silent.reset();

        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"restarted"};
        });
        auto restarted = Exchange(pipeName, MakeRequest(50));
        Check(restarted && restarted->Payload == L"restarted", "server must support Start after Stop");

        auto active = OpenClient(pipeName);
        std::vector<std::jthread> stoppers;
        for (int index = 0; index < 4; ++index)
            stoppers.emplace_back([&] { server.Stop(); });
        stoppers.clear();
        Check(!server.IsRunning(), "concurrent Stop calls must be idempotent");
    }

    {
        auto options = TestOptions(L"stop-handler", 1);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        Event returned;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            server.Stop();
            server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
                return apc::control::Response{apc::control::ExitCode::Success, L"must-not-start-reentrantly"};
            });
            returned.Signal();
            return apc::control::Response{apc::control::ExitCode::Success, L"ignored"};
        });
        auto pipe = OpenClient(pipeName);
        auto request = MakeRequest(51);
        Check(pipe && WriteRequest(pipe.get(), request), "reentrant-stop request must reach the handler");
        Check(returned.Wait(1000), "Stop and Start called from a handler must return without self-deadlock");
        Check(WaitUntil([&] { return !server.IsRunning(); }, 1000), "reentrant Stop must linearize immediately");
        pipe.reset();
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"after-stop"};
        });
        auto response = Exchange(pipeName, MakeRequest(52));
        Check(response && response->Payload == L"after-stop",
              "deferred Stop finalizer must leave a restartable server");
        server.Stop();
    }

    {
        auto options = TestOptions(L"early-disconnect-rearm", 1);
        const auto pipeName = options.PipeName;
        std::atomic_bool injected = false;
        std::atomic_bool earlyClientOpened = false;
        options.ConnectPipe = [&](HANDLE pipe, LPOVERLAPPED operation) noexcept {
            if (injected.exchange(true)) return ConnectNamedPipe(pipe, operation);
            wil::unique_handle client(CreateFileW(pipeName.c_str(),
                                                  GENERIC_READ | FILE_WRITE_DATA,
                                                  0,
                                                  nullptr,
                                                  OPEN_EXISTING,
                                                  FILE_FLAG_OVERLAPPED,
                                                  nullptr));
            earlyClientOpened = static_cast<bool>(client);
            client.reset();
            return ConnectNamedPipe(pipe, operation);
        };
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"rearmed-after-no-data"};
        });
        Check(earlyClientOpened.load(), "the pre-ConnectNamedPipe client must exercise ERROR_NO_DATA");
        auto response = Exchange(pipeName, MakeRequest(55), true, 1500);
        Check(response && response->Payload == L"rearmed-after-no-data",
              "ERROR_NO_DATA must disconnect and rearm the slot without prolonged backoff");
        server.Stop();
    }

    {
        auto options = TestOptions(L"rearm-retry", 1);
        const auto pipeName = options.PipeName;
        options.RetryDelayMs = 1;
        std::atomic_int attempts = 0;
        options.ConnectPipe = [&](HANDLE pipe, LPOVERLAPPED operation) noexcept -> BOOL {
            if (++attempts <= 8) {
                SetLastError(ERROR_RETRY);
                return FALSE;
            }
            return ConnectNamedPipe(pipe, operation);
        };
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"recovered"};
        });
        Check(server.IsRunning(), "server lifecycle must stay healthy while an individual slot retries");
        auto stale = OpenClient(pipeName);
        Check(static_cast<bool>(stale), "unarmed fixture client must attach to the old pipe instance");
        if (stale) {
            Check(WaitForServerDisconnect(stale.get(), 3000),
                  "slot recreation must close clients attached to the invalid old instance");
        }
        stale.reset();
        auto response = Exchange(pipeName, MakeRequest(53), true, 2000);
        Check(response && response->Payload == L"recovered", "failed arm must recover with bounded backoff");
        Check(attempts.load() >= 8, "persistent arm failures must exercise slot recreation");
        server.Stop();
    }

    {
        auto options = TestOptions(L"restart-dedupe", 1);
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        std::atomic_int firstHandlerCalls = 0;
        std::atomic_int replacementHandlerCalls = 0;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) -> apc::control::Response {
            ++firstHandlerCalls;
            throw std::runtime_error("failure after a possible side effect");
        });
        auto request = MakeRequest(54);
        auto firstResponse = Exchange(pipeName, request, false);
        Check(firstResponse && firstResponse->Code == apc::control::ExitCode::Indeterminate,
              "a handler exception after a possible side effect must remain indeterminate");
        server.Stop();

        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++replacementHandlerCalls;
            return apc::control::Response{apc::control::ExitCode::Success, L"executed-again"};
        });
        auto replay = Exchange(pipeName, request);
        Check(replay && replay->Code == apc::control::ExitCode::Indeterminate,
              "same-process endpoint restart must replay the cached canonical result");
        Check(firstHandlerCalls.load() == 1 && replacementHandlerCalls.load() == 0,
              "same-process endpoint restart must preserve exactly-once execution");
        server.Stop();
    }
}

void TestHandlerCaptureDestructionOutsideLifecycleLock() {
    CommandLineControlServer server(TestOptions(L"handler-capture-stop", 1));
    Event destructorEntered, allowDestruction, released, startEntered, startReturned;
    auto capture = std::shared_ptr<int>(new int, [&](int* value) noexcept {
        delete value;
        server.Stop();
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"must-not-start-during-stop"};
        });
        destructorEntered.Signal();
        static_cast<void>(allowDestruction.Wait(5000));
        released.Signal();
    });
    server.Start([capture = std::move(capture)](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"unused"};
    });
    Check(server.IsRunning(), "capture-destruction fixture must start the pipe server");

    std::thread stopper([&] { server.Stop(); });
    const bool entered = destructorEntered.Wait(5000);
    Check(entered, "handler capture destruction must reenter Stop and Start without a lifecycle self-wait");
    if (!entered) std::terminate();
    std::thread starter([&] {
        startEntered.Signal();
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"after-stop"};
        });
        startReturned.Signal();
    });
    Check(startEntered.Wait(2000), "concurrent restart must enter during handler capture destruction");
    Check(!startReturned.Wait(100), "a concurrent Start must wait for handler capture destruction");
    allowDestruction.Signal();
    const bool completed = released.Wait(5000);
    Check(completed, "handler capture destruction must finish before another Start proceeds");
    if (!completed) std::terminate();
    stopper.join();
    starter.join();
    Check(server.IsRunning(), "a concurrent Start must resume after handler capture destruction");
    server.Stop();
}

void TestStopNotificationOutsideLifecycleLock() {
    auto options = TestOptions(L"stop-callback-reentrancy", 1);
    auto const pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    Event registered, callbackReturned, release;
    server.Start([&](apc::control::Request const&, std::stop_token token, std::uint64_t) {
        std::stop_callback callback(token, [&] {
            server.RequestStop();
            callbackReturned.Signal();
        });
        registered.Signal();
        static_cast<void>(release.Wait(5000));
        return apc::control::Response{apc::control::ExitCode::Success, L"stopped"};
    });
    auto client = OpenClient(pipeName);
    Check(client && WriteRequest(client.get(), MakeRequest(62)), "stop-callback fixture must reach the handler");
    Check(registered.Wait(2000), "handler must register its stop callback before cancellation");

    std::thread requester([&] { server.RequestStop(); });
    const bool notified = callbackReturned.Wait(5000);
    Check(notified, "stop callback must reenter RequestStop without holding the lifecycle lock");
    release.Signal();
    if (!notified) std::terminate();
    requester.join();
    client.reset();
    server.Stop();
}

void TestStartupSquattingAndSlotRecovery() {
    auto options = TestOptions(L"squatting", 1);
    const auto pipeName = options.PipeName;
    wil::unique_handle squatter(CreateNamedPipeW(pipeName.c_str(),
                                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                 1,
                                                 1024,
                                                 1024,
                                                 0,
                                                 nullptr));
    Check(static_cast<bool>(squatter), "squatting fixture must own the predictable pipe name");

    CommandLineControlServer server(std::move(options));
    server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    });
    Check(!server.IsRunning(), "pipe squatting must degrade only the optional control endpoint");
    server.Stop();
    squatter.reset();

    server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    });
    Check(server.IsRunning(), "server must start after the conflicting pipe disappears");
    server.Stop();

    {
        auto partialOptions = TestOptions(L"partial-squatting", 2);
        const auto partialPipeName = partialOptions.PipeName;
        const auto blockedSlotName = apc::control::PipeInstanceName(partialPipeName, 1);
        wil::unique_handle blockedSlot(CreateNamedPipeW(blockedSlotName.c_str(),
                                                        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                        1,
                                                        1024,
                                                        1024,
                                                        0,
                                                        nullptr));
        Check(static_cast<bool>(blockedSlot), "one-slot squatting fixture must be created");
        CommandLineControlServer partialServer(std::move(partialOptions));
        partialServer.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"available-slot"};
        });
        Check(partialServer.IsRunning(), "one blocked slot must not disable every healthy control slot");
        auto partialResponse = Exchange(partialPipeName, MakeRequest(60));
        Check(partialResponse && partialResponse->Payload == L"available-slot",
              "a healthy slot must serve commands while another slot is blocked");
        blockedSlot.reset();
        wil::unique_handle recoveredClient;
        const auto recoveredDeadline = GetTickCount64() + 3000;
        do {
            recoveredClient.reset(CreateFileW(blockedSlotName.c_str(),
                                              GENERIC_READ | FILE_WRITE_DATA,
                                              0,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_FLAG_OVERLAPPED,
                                              nullptr));
            if (recoveredClient) break;
            Sleep(5);
        } while (GetTickCount64() < recoveredDeadline);
        const auto recoveredRequest = MakeRequest(61);
        Check(recoveredClient && WriteRequest(recoveredClient.get(), recoveredRequest),
              "the specifically recovered slot must accept a complete request");
        auto recoveredResponse =
            recoveredClient ? ReadResponse(recoveredClient.get(), recoveredRequest.CorrelationId) : std::nullopt;
        Check(recoveredResponse && recoveredResponse->Payload == L"available-slot",
              "the specifically recovered slot must deliver its response and ACK path");
        partialServer.Stop();
    }

    auto derivedPipeName = apc::control::PipeName();
    Check(derivedPipeName && derivedPipeName->starts_with(LR"(\\.\pipe\AudioPlaybackConnector2.Control.v2.)"),
          "pipe namespace derivation must succeed without fallback identity");
}

void TestStartStopHandleStability() {
    auto options = TestOptions(L"leaks", 1);
    const auto pipeName = options.PipeName;
    // The close must follow the ACK, not an expired acknowledgement deadline.
    options.AcknowledgementTimeoutMs = 5000;
    CommandLineControlServer server(std::move(options));
    const auto handler = [](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    };

    for (std::uint64_t cycle = 0; cycle < 5; ++cycle) {
        server.Start(handler);
        Check(CompleteRoundTrip(pipeName, MakeRequest(100 + cycle)),
              "warmup exchange must reach server disconnect before stopping the endpoint");
        server.Stop();
    }
    DWORD before = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "handle baseline must be readable");

    for (std::uint64_t cycle = 0; cycle < 100; ++cycle) {
        server.Start(handler);
        Check(CompleteRoundTrip(pipeName, MakeRequest(1000 + cycle)),
              "stress exchange must reach server disconnect before stopping the endpoint");
        server.Stop();
    }
    DWORD after = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "handle result must be readable");
    Check(after <= before + 3, "100 server lifecycles must not leak kernel handles");
}

void TestMaximumRequestAndIdleCachePruning() {
    {
        auto options = TestOptions(L"maximum-request", 1);
        options.RequestTimeoutMs = 3000;
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        std::atomic_bool receivedIntact = false;
        server.Start([&](apc::control::Request const& request, std::stop_token, std::uint64_t) {
            receivedIntact = request.Payload.size() == apc::control::c_maxPayloadBytes / sizeof(wchar_t) &&
                             std::ranges::all_of(request.Payload, [](wchar_t value) { return value == L'z'; });
            return apc::control::Response{apc::control::ExitCode::Success, L"accepted"};
        });
        auto request = MakeRequest(300, apc::control::CommandType::Connect);
        request.Target = apc::control::TargetKind::Name;
        request.Payload.assign(apc::control::c_maxPayloadBytes / sizeof(wchar_t), L'z');
        auto response = Exchange(pipeName, request, true, 4000);
        Check(response && response->Payload == L"accepted" && receivedIntact.load(),
              "maximum-size requests must survive 4 KiB pipe backpressure without corruption");
        server.Stop();
    }

    for (bool acknowledge : {true, false}) {
        auto options = TestOptions(L"cache-idle-prune", 1);
        options.AcknowledgedRecordLifetime = 30ms;
        options.RequestRecordLifetime = 60ms;
        options.MaxRequestCacheBytes = 1; // Clamp to exactly one maximum request/response reservation.
        std::atomic<std::int64_t> elapsedMs = 0;
        options.CacheNow = [&]() noexcept {
            return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(elapsedMs.load());
        };
        Event timerArmed;
        Event timerIdle;
        std::atomic_bool wasArmed = false;
        options.SetCacheTimer = [&](PTP_TIMER timer, FILETIME* due) noexcept {
            SetThreadpoolTimer(timer, due, 0, 0);
            if (due) {
                wasArmed = true;
                timerArmed.Signal();
            } else if (wasArmed.load()) {
                timerIdle.Signal();
            }
        };
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        std::atomic_int handlerCalls = 0;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++handlerCalls;
            return apc::control::Response{apc::control::ExitCode::Success, L"cached"};
        });
        const auto request = MakeRequest(301);
        auto response = Exchange(pipeName, request, acknowledge);
        Check(response && response->Payload == L"cached", "expiry fixture must complete its first request");
        Check(timerArmed.Wait(2000), "completed delivery must schedule cache expiry");
        Check(!timerIdle.Wait(80), "native timer must retain the record while the cache clock has not advanced");
        // No further client request or Stop can trigger pruning before this wait.
        elapsedMs = acknowledge ? 31 : 61;
        Check(timerIdle.Wait(2000), "idle timer must prune the expired record and disarm itself");
        auto capacityProbe = MakeRequest(302, apc::control::CommandType::Connect);
        capacityProbe.Target = apc::control::TargetKind::Name;
        capacityProbe.Payload.assign(apc::control::c_maxPayloadBytes / sizeof(wchar_t), L'z');
        Check(CompleteRoundTrip(pipeName, capacityProbe),
              "idle pruning must restore the entire byte budget before the next request");
        auto replay = Exchange(pipeName, request, acknowledge);
        Check(replay && replay->Payload == L"cached" && handlerCalls.load() == 3,
              "expired correlation must execute again for acknowledged and unacknowledged results");
        server.Stop();
    }
}

void TestStrictCommandSemantics() {
    auto alias = MakeRequest(200, apc::control::CommandType::AliasSet);
    alias.Target = apc::control::TargetKind::Id;
    alias.Payload = L"device\nalias";
    Check(apc::control::IsRequestValid(alias), "well-formed alias-set payload must be valid");
    alias.Payload = L"device\n";
    Check(!apc::control::IsRequestValid(alias), "empty alias value must be rejected before the handler");
    alias.Payload = L"device\nalias\nextra";
    Check(!apc::control::IsRequestValid(alias), "multi-line alias values must be rejected before the handler");
    alias.Payload = L"device\nalias";
    alias.Target = apc::control::TargetKind::Default;
    Check(!apc::control::IsRequestValid(alias), "alias-set must reject unsupported default targets");

    auto defaultSet = MakeRequest(201, apc::control::CommandType::DefaultSet);
    defaultSet.Target = apc::control::TargetKind::Last;
    Check(!apc::control::IsRequestValid(defaultSet), "default-set must require an explicit resolvable target");
}

void TestProductionTrustRejectsUnpackagedClientBeforeHandler() {
    UINT32 packageLength = 0;
    Check(GetCurrentPackageFamilyName(&packageLength, nullptr) == APPMODEL_ERROR_NO_PACKAGE,
          "the bounded test host must run without package identity");
    auto expectedIdentity = apc::control::ProcessExecutableIdentity(GetCurrentProcess());
    Check(expectedIdentity.has_value(), "the negative trust test must resolve its own executable identity");
#if defined(_DEBUG)
    // Debug builds intentionally support exact-file development peers. Missing
    // identity remains forbidden in that configuration; Release rejects both.
    expectedIdentity.reset();
#endif
    auto options = TestOptions(L"production-trust-rejection", 1);
    Event trustChecked;
    options.IsTrustedClient = [expectedIdentity, &trustChecked](HANDLE pipe) noexcept {
        auto const trusted = apc::control::IsTrustedNamedPipeClient(pipe, expectedIdentity);
        trustChecked.Signal();
        return trusted;
    };
    auto pipeName = options.PipeName;
    CommandLineControlServer server(std::move(options));
    std::atomic_uint32_t handlerCalls = 0;
    server.Start([&](auto const&, std::stop_token, std::uint64_t) {
        ++handlerCalls;
        return apc::control::Response{apc::control::ExitCode::Success, L"must not run"};
    });
    Check(server.IsRunning(), "production trust fixture must start");
    auto response = Exchange(pipeName, MakeRequest(401));
    Check(trustChecked.Wait(2000), "the connected client must pass through the production trust check");
    Check(!response && handlerCalls == 0, "an untrusted peer must receive no handler result or execute a command");
    server.Stop();
}
} // namespace

int RunCommandLineControlServerTests() {
    TestProductionRoundTripFragmentationAndRearm();
    TestDisconnectBeforeResponseRetriesExactlyOnce();
    TestDisconnectAfterResponseBeforeAckRetriesExactlyOnce();
    TestParallelDuplicatesAndCorrelationConflict();
    TestMalformedTimeoutOversizeAndRecovery();
    TestRequestStopCancelsControllerWorkBeforeDrain();
    TestStopLifecycleAndRearmRetry();
    TestHandlerCaptureDestructionOutsideLifecycleLock();
    TestStopNotificationOutsideLifecycleLock();
    TestStartupSquattingAndSlotRecovery();
    TestStartStopHandleStability();
    TestMaximumRequestAndIdleCachePruning();
    TestStrictCommandSemantics();
    TestProductionTrustRejectsUnpackagedClientBeforeHandler();
    return g_failures;
}
