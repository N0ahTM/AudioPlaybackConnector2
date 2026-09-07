#include <control/CommandPipeSecurity.hpp>
#include <control/CommandProtocol.hpp>
#include <app/LegacyAppUseCaseBridge.hpp>
#include <services/CommandLineControlServer.hpp>

#include <aclapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using apc::app::LegacyAppUseCaseBridge;

int g_failures = 0;
std::atomic_uint64_t g_pipeSequence = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value);
        m_value = value;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_value = nullptr;
};

class Event {
public:
    Event() : m_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!m_handle) throw std::runtime_error("CreateEventW failed");
    }
    ~Event() { CloseHandle(m_handle); }
    Event(Event const&) = delete;
    Event& operator=(Event const&) = delete;

    void Signal() const noexcept { SetEvent(m_handle); }
    [[nodiscard]] bool Wait(DWORD timeoutMs) const noexcept {
        return WaitForSingleObject(m_handle, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    HANDLE m_handle = nullptr;
};

std::wstring UniquePipeName(std::wstring_view testName) {
    return L"\\\\.\\pipe\\AudioPlaybackConnector2.ServerTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(++g_pipeSequence) + L"." + std::wstring(testName);
}

CommandLineControlServer::Options TestOptions(std::wstring_view testName, std::size_t instances = 2) {
    CommandLineControlServer::Options options;
    options.PipeName = UniquePipeName(testName);
    options.PipeInstanceCount = instances;
    options.RequestTimeoutMs = 500;
    options.HandlerTimeoutMs = 2000;
    options.ResponseTimeoutMs = 500;
    options.AcknowledgementTimeoutMs = 100;
    options.RetryDelayMs = 10;
    options.RequestRecordLifetime = 2s;
    options.RetryStartupFailures = false;
    options.IsTrustedClient = [](HANDLE) noexcept { return true; };
    return options;
}

UniqueHandle OpenClient(std::wstring const& pipeName, DWORD timeoutMs = 2000) {
    const auto deadline = GetTickCount64() + timeoutMs;
    DWORD baseError = ERROR_SUCCESS;
    while (true) {
        for (std::size_t index = 0; index < apc::control::c_pipeInstanceCount; ++index) {
            const auto instanceName = apc::control::PipeInstanceName(pipeName, index);
            UniqueHandle pipe(CreateFileW(instanceName.c_str(),
                                          GENERIC_READ | FILE_WRITE_DATA,
                                          0,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED,
                                          nullptr));
            if (pipe) return pipe;
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

bool WriteRequest(HANDLE pipe, apc::control::Request const& request, DWORD timeoutMs = 1000) {
    return apc::control::WriteRequest(pipe, request, nullptr, apc::control::DeadlineAfter(timeoutMs)) ==
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
    if (!pipe || !WriteRequest(pipe.Get(), request, timeoutMs)) return std::nullopt;
    return ReadResponse(pipe.Get(), request.CorrelationId, acknowledge, timeoutMs);
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
    options.IsTrustedClient = {};
    auto currentImage = apc::control::details::ProcessImagePath(GetCurrentProcess());
    Check(static_cast<bool>(currentImage), "production trust test must resolve its client executable");
    if (!currentImage) return;
    auto currentIdentity = apc::control::ExecutableIdentityFromPath(*currentImage);
    Check(static_cast<bool>(currentIdentity), "production trust test must resolve its client file identity");
    if (!currentIdentity) return;
    options.IsTrustedClient = [currentIdentity](HANDLE pipe) noexcept {
        return apc::control::IsTrustedNamedPipeClient(pipe, currentIdentity);
    };
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
        Check(apc::control::WriteExact(pipe.Get(), headerBytes, 7, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented header prefix must be accepted");
        Check(apc::control::WriteExact(pipe.Get(), headerBytes + 7, sizeof(header) - 7, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented header suffix must be accepted");
        auto* payloadBytes = reinterpret_cast<std::byte*>(request.Payload.data());
        Check(apc::control::WriteExact(pipe.Get(), payloadBytes, 3, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented payload prefix must be accepted");
        Check(apc::control::WriteExact(pipe.Get(), payloadBytes + 3, header.PayloadBytes - 3, nullptr, deadline) ==
                  apc::control::IoStatus::Success,
              "fragmented payload suffix must be accepted");
        auto response = ReadResponse(pipe.Get(), request.CorrelationId);
        Check(response && response->Payload == L"echo:device-id", "fragmented request must reach the real handler");
    }
    pipe.Reset();

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
    Check(abandoned && WriteRequest(abandoned.Get(), request), "abandoning client must send its complete request");
    Check(started.Wait(1000), "handler must start before the first client disconnects");
    abandoned.Reset();
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
        Check(first && second && WriteRequest(first.Get(), request) && WriteRequest(second.Get(), request),
              "parallel duplicate clients must send the same request");
        Check(started.Wait(1000), "one parallel handler must start");
        release.Signal();
        auto firstResponse = ReadResponse(first.Get(), request.CorrelationId);
        auto secondResponse = ReadResponse(second.Get(), request.CorrelationId);
        Check(firstResponse && secondResponse && firstResponse->Payload == secondResponse->Payload,
              "parallel duplicates must receive the same response");
        Check(calls.load() == 1, "parallel duplicate requests must execute once");
        server.Stop();
    }

    {
        auto options = TestOptions(L"parallel-cache-pressure", 2);
        options.MaxRequestRecords = 1;
        options.AcknowledgedRecordLifetime = 100ms;
        const auto pipeName = options.PipeName;
        Event secondPromotionReached;
        Event releaseSecondPromotion;
        std::atomic_int promotions = 0;
        options.BeforeDeliveryPromoted = [&](std::size_t) noexcept {
            if (++promotions == 2) {
                secondPromotionReached.Signal();
                (void)releaseSecondPromotion.Wait(3000);
            }
        };
        CommandLineControlServer server(std::move(options));
        Event handlerStarted;
        Event releaseHandler;
        std::atomic_int calls = 0;
        server.Start([&](apc::control::Request const&, std::stop_token, std::uint64_t) {
            ++calls;
            handlerStarted.Signal();
            (void)releaseHandler.Wait(1500);
            return apc::control::Response{apc::control::ExitCode::Success, L"retained"};
        });

        auto request = MakeRequest(32);
        auto first = OpenClient(pipeName);
        auto second = OpenClient(pipeName);
        Check(first && second && WriteRequest(first.Get(), request) && WriteRequest(second.Get(), request),
              "cache-pressure duplicates must both be accepted");
        Check(handlerStarted.Wait(1000), "cache-pressure canonical handler must start");
        releaseHandler.Signal();
        Check(secondPromotionReached.Wait(1000), "one duplicate delivery must remain queued before promotion");

        std::optional<apc::control::Response> firstResponse;
        std::optional<apc::control::Response> secondResponse;
        Event oneResponseRead;
        std::jthread firstReader([&] {
            firstResponse = ReadResponse(first.Get(), request.CorrelationId, true, 3000);
            oneResponseRead.Signal();
        });
        std::jthread secondReader([&] {
            secondResponse = ReadResponse(second.Get(), request.CorrelationId, true, 3000);
            oneResponseRead.Signal();
        });
        Check(oneResponseRead.Wait(1000), "one duplicate must receive and acknowledge the canonical response");

        auto pressure = Exchange(pipeName, MakeRequest(33), true, 1000);
        Check(pressure && pressure->Code == apc::control::ExitCode::Busy,
              "cache pressure must not evict a record with a queued duplicate");
        releaseSecondPromotion.Signal();
        firstReader.join();
        secondReader.join();
        Check(firstResponse && secondResponse && firstResponse->Payload == L"retained" &&
                  secondResponse->Payload == L"retained",
              "both duplicates must receive the retained response after cache pressure");
        Check(calls.load() == 1, "cache pressure must not re-execute a queued duplicate");
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
        Check(originalPipe && WriteRequest(originalPipe.Get(), original), "canonical request must be sent");
        Check(started.Wait(1000), "canonical handler must be in flight");
        auto conflict = Exchange(pipeName, conflicting);
        Check(conflict && conflict->Code == apc::control::ExitCode::InvalidRequest,
              "same correlation with a different request must be rejected");
        release.Signal();
        auto originalResponse = ReadResponse(originalPipe.Get(), original.CorrelationId, false);
        Check(originalResponse && originalResponse->Payload == L"canonical",
              "canonical response must survive conflict");
        originalPipe.Reset();
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
                  malformed.Get(), &invalidHeader, sizeof(invalidHeader), nullptr, apc::control::DeadlineAfter(500)) ==
                  apc::control::IoStatus::Success,
          "malformed header must reach the server");
    malformed.Reset();

    auto silent = OpenClient(pipeName);
    auto validHeader = apc::control::RequestHeader{};
    validHeader.CorrelationHigh = 2;
    validHeader.CorrelationLow = 2;
    validHeader.Command = static_cast<std::uint32_t>(apc::control::CommandType::Show);
    Check(silent &&
              apc::control::WriteExact(
                  silent.Get(), &validHeader, sizeof(validHeader) / 2, nullptr, apc::control::DeadlineAfter(500)) ==
                  apc::control::IoStatus::Success,
          "partial header must be accepted before timeout");
    Sleep(120);
    std::byte byte{};
    const auto closed = apc::control::ReadExact(silent.Get(), &byte, 1, nullptr, apc::control::DeadlineAfter(500));
    Check(closed == apc::control::IoStatus::Closed || closed == apc::control::IoStatus::Cancelled ||
              closed == apc::control::IoStatus::Failed,
          "partial client must be disconnected at the absolute request deadline");
    silent.Reset();

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

void TestRequestStopCancelsLeasedBridgeWorkBeforeBridgeDrain() {
    auto options = TestOptions(L"request-stop-bridge-drain", 1);
    const auto pipeName = options.PipeName;

    SettingsData settings;
    settings.Devices.push_back(DeviceSettings{L"device-id", L"Device", {}, false, false});
    std::mutex operationMutex;
    std::condition_variable operationChanged;
    Event operationEntered;
    std::atomic_bool cancellationObserved = false;
    std::atomic_bool releaseForFailedTest = false;
    std::atomic_int mutations = 0;
    std::atomic_int settingsUiCalls = 0;
    const SettingsSnapshot settingsSnapshot{settings, 1, false};
    LegacyAppUseCaseBridge::Operations operations;
    operations.ReadSettings = [&] { return settingsSnapshot; };
    operations.ReadConnectedDevices = [] {
        return std::vector<LegacyAppUseCaseBridge::DeviceRecord>{
            {L"device-id", L"Device", {}, apc::app::DeviceConnectionState::Idle, false, true, false}};
    };
    operations.Connect = [&](std::wstring_view, apc::app::AppCommandContext const& context) {
        operationEntered.Signal();
        std::stop_callback cancellationWake{context.StopToken, [&] { operationChanged.notify_all(); }};
        std::unique_lock lock(operationMutex);
        operationChanged.wait(lock, [&] {
            return context.IsCancellationRequested() || releaseForFailedTest.load(std::memory_order_acquire);
        });
        if (context.IsCancellationRequested()) {
            cancellationObserved = true;
            return LegacyAppUseCaseBridge::OperationResult{LegacyAppUseCaseBridge::OperationStatus::Cancelled};
        }
        ++mutations;
        return LegacyAppUseCaseBridge::OperationResult{LegacyAppUseCaseBridge::OperationStatus::Failed};
    };
    operations.ShowSettings = [&](apc::app::AppCommandContext const&) {
        ++settingsUiCalls;
        return LegacyAppUseCaseBridge::UiActionResult{LegacyAppUseCaseBridge::OperationStatus::Succeeded, std::nullopt};
    };
    LegacyAppUseCaseBridge bridge(std::move(operations));

    CommandLineControlServer server(std::move(options));
    server.Start([&](apc::control::Request const& request, std::stop_token stopToken, std::uint64_t) {
        apc::app::AppCommandContext context;
        context.StopToken = stopToken;
        const auto result = bridge.Execute(
            apc::app::AppCommand{apc::app::AppCommandKind::Connect, apc::app::DeviceSelector::ById(request.Payload)},
            context);
        return apc::control::Response{result.Code == apc::app::AppResultCode::Cancelled
                                          ? apc::control::ExitCode::Indeterminate
                                          : apc::control::ExitCode::OperationFailed,
                                      L""};
    });

    auto client = OpenClient(pipeName);
    auto request = MakeRequest(61, apc::control::CommandType::Connect);
    request.Target = apc::control::TargetKind::Id;
    request.Payload = L"device-id";
    Check(client && WriteRequest(client.Get(), request), "leased request must reach the control handler");
    Check(operationEntered.Wait(1000), "bridge operation must begin before teardown requests cancellation");

    Event teardownReturned;
    std::jthread teardown([&] {
        // This is the production ordering in ApplicationHost::PerformTeardown:
        // first signal the P01 handler, then wait for bridge admission to drain.
        server.RequestStop();
        bridge.SetRunning(false);
        teardownReturned.Signal();
    });
    const bool teardownCompleted = teardownReturned.Wait(1000);
    Check(teardownCompleted,
          "request-stop cancellation must release the admitted bridge lease before teardown can drain it");
    if (!teardownCompleted) {
        releaseForFailedTest.store(true, std::memory_order_release);
        operationChanged.notify_all();
    }
    teardown.join();

    Check(cancellationObserved.load() && mutations.load() == 0,
          "a cancelled leased operation must not mutate state after teardown begins");
    Check(!server.IsRunning(), "RequestStop must close P01 admission before bridge teardown");
    const auto lateFact = bridge.Observe({LegacyAppUseCaseBridge::FactKind::DeviceStatusChanged,
                                          L"device-id",
                                          apc::app::DeviceConnectionState::Connecting,
                                          apc::app::AppResultCode::OperationFailed});
    const auto lateUi = bridge.Execute(apc::app::AppCommand{apc::app::AppCommandKind::ShowSettings});
    Check(!lateFact && lateUi.Code == apc::app::AppResultCode::Unavailable && settingsUiCalls.load() == 0,
          "after bridge drain no late fact, mutation, or UI callback may run");

    client.Reset();
    server.Stop();
}

void TestStopLifecycleAndRearmRetry() {
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
        silent.Reset();

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
        Check(pipe && WriteRequest(pipe.Get(), request), "reentrant-stop request must reach the handler");
        Check(returned.Wait(1000), "Stop and Start called from a handler must return without self-deadlock");
        Check(WaitUntil([&] { return !server.IsRunning(); }, 1000), "reentrant Stop must linearize immediately");
        pipe.Reset();
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
        options.BeforeArmConnection = [&](std::size_t) noexcept {
            if (injected.exchange(true)) return true;
            UniqueHandle client(CreateFileW(pipeName.c_str(),
                                            GENERIC_READ | FILE_WRITE_DATA,
                                            0,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_FLAG_OVERLAPPED,
                                            nullptr));
            earlyClientOpened = static_cast<bool>(client);
            return true;
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
        Event recreated;
        options.BeforeArmConnection = [&](std::size_t) noexcept {
            ++attempts;
            return false;
        };
        options.AfterPipeRecreated = [&](std::size_t) noexcept { recreated.Signal(); };
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"recovered"};
        });
        Check(server.IsRunning(), "server lifecycle must stay healthy while an individual slot retries");
        auto stale = OpenClient(pipeName);
        Check(static_cast<bool>(stale), "unarmed fixture client must attach to the old pipe instance");
        Check(recreated.Wait(2000), "persistent arm failures must recreate the affected slot");
        if (stale) {
            std::byte byte{};
            const auto closed =
                apc::control::ReadExact(stale.Get(), &byte, 1, nullptr, apc::control::DeadlineAfter(500));
            Check(closed != apc::control::IoStatus::Success,
                  "slot recreation must close clients attached to the invalid old instance");
        }
        stale.Reset();
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

void TestStartupSquattingAndSecurityDescriptor() {
    auto options = TestOptions(L"squatting", 1);
    const auto pipeName = options.PipeName;
    UniqueHandle squatter(CreateNamedPipeW(pipeName.c_str(),
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
    squatter.Reset();

    server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    });
    Check(server.IsRunning(), "server must start after the conflicting pipe disappears");
    server.Stop();

    {
        auto partialOptions = TestOptions(L"partial-squatting", 2);
        const auto partialPipeName = partialOptions.PipeName;
        const auto blockedSlotName = apc::control::PipeInstanceName(partialPipeName, 1);
        UniqueHandle blockedSlot(CreateNamedPipeW(blockedSlotName.c_str(),
                                                  PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                  1,
                                                  1024,
                                                  1024,
                                                  0,
                                                  nullptr));
        Check(static_cast<bool>(blockedSlot), "one-slot squatting fixture must be created");
        Event recoveredSlot;
        partialOptions.AfterPipeRecreated = [&](std::size_t index) noexcept {
            if (index == 1) recoveredSlot.Signal();
        };
        CommandLineControlServer partialServer(std::move(partialOptions));
        partialServer.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"available-slot"};
        });
        Check(partialServer.IsRunning(), "one blocked slot must not disable every healthy control slot");
        auto partialResponse = Exchange(partialPipeName, MakeRequest(60));
        Check(partialResponse && partialResponse->Payload == L"available-slot",
              "a healthy slot must serve commands while another slot is blocked");
        blockedSlot.Reset();
        Check(recoveredSlot.Wait(3000), "a blocked startup slot must recover after its namespace becomes available");
        UniqueHandle recoveredClient;
        const auto recoveredDeadline = GetTickCount64() + 1000;
        do {
            recoveredClient.Reset(CreateFileW(blockedSlotName.c_str(),
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
        Check(recoveredClient && WriteRequest(recoveredClient.Get(), recoveredRequest),
              "the specifically recovered slot must accept a complete request");
        auto recoveredResponse =
            recoveredClient ? ReadResponse(recoveredClient.Get(), recoveredRequest.CorrelationId) : std::nullopt;
        Check(recoveredResponse && recoveredResponse->Payload == L"available-slot",
              "the specifically recovered slot must deliver its response and ACK path");
        partialServer.Stop();
    }

    auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
    Check(security && security->Get() && security->Get()->bInheritHandle == FALSE,
          "pipe security attributes must be non-inheritable");
    if (security && security->Get()) {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        Check(GetSecurityDescriptorControl(security->Get()->lpSecurityDescriptor, &control, &revision) != FALSE &&
                  (control & SE_DACL_PROTECTED) != 0,
              "pipe DACL must be protected from inherited broad ACEs");
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        Check(GetSecurityDescriptorDacl(security->Get()->lpSecurityDescriptor, &present, &dacl, &defaulted) != FALSE &&
                  present && dacl,
              "pipe security descriptor must contain a non-null DACL");
        if (dacl) {
            Check(dacl->AceCount == 1, "pipe DACL must contain only the current-user allow ACE");
            void* rawAce = nullptr;
            Check(dacl->AceCount == 1 && GetAce(dacl, 0, &rawAce) != FALSE && rawAce,
                  "pipe DACL must expose its only ACE");
            if (rawAce) {
                auto const* ace = static_cast<ACCESS_ALLOWED_ACE const*>(rawAce);
                Check(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE,
                      "pipe DACL must grant rather than deny the current user");
                constexpr ACCESS_MASK clientAccess = GENERIC_READ | FILE_WRITE_DATA;
                Check(ace->Mask == clientAccess, "pipe DACL must grant client I/O without FILE_CREATE_PIPE_INSTANCE");
                Check((ace->Mask & FILE_CREATE_PIPE_INSTANCE) == 0,
                      "pipe clients must not be able to create rogue server instances");
                auto const* self = apc::control::details::CurrentProcessIdentity();
                auto* aceSid = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
                Check(self && IsValidSid(aceSid) &&
                          EqualSid(reinterpret_cast<PSID>(const_cast<std::byte*>(self->UserSid.data())), aceSid) !=
                              FALSE,
                      "pipe DACL ACE must target the current user SID");
            }
        }

        const auto protectedName = UniquePipeName(L"protected-instance");
        UniqueHandle protectedPipe(CreateNamedPipeW(protectedName.c_str(),
                                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                    PIPE_UNLIMITED_INSTANCES,
                                                    1024,
                                                    1024,
                                                    0,
                                                    security->Get()));
        Check(static_cast<bool>(protectedPipe), "protected pipe fixture must be created");
        UniqueHandle client(
            CreateFileW(protectedName.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        Check(static_cast<bool>(client), "the restricted DACL must still permit client read/write access");
        SetLastError(ERROR_SUCCESS);
        UniqueHandle rogue(CreateNamedPipeW(protectedName.c_str(),
                                            PIPE_ACCESS_DUPLEX,
                                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                            PIPE_UNLIMITED_INSTANCES,
                                            1024,
                                            1024,
                                            0,
                                            nullptr));
        Check(!rogue && GetLastError() == ERROR_ACCESS_DENIED,
              "the restricted DACL must reject same-user rogue server instances");
    }
    auto derivedPipeName = apc::control::PipeName();
    Check(derivedPipeName && derivedPipeName->starts_with(apc::control::c_pipeNamePrefix),
          "pipe namespace derivation must succeed without fallback identity");
    Check(apc::control::IsTrustedPeerProcess(GetCurrentProcessId()), "current process identity must trust itself");
    Check(apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId()),
          "an already-open current-process handle must trust itself without reopening the process");
    auto currentImage = apc::control::details::ProcessImagePath(GetCurrentProcess());
    auto currentIdentity = currentImage ? apc::control::ExecutableIdentityFromPath(*currentImage) : std::nullopt;
    Check(currentIdentity &&
              apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId(), currentIdentity),
          "strict unpackaged trust must accept the exact current executable image");
    if (currentIdentity) {
        auto differentIdentity = *currentIdentity;
        differentIdentity.FileId.Identifier[0] ^= 1;
        Check(!apc::control::IsTrustedPeerProcess(
                  GetCurrentProcess(), GetCurrentProcessId(), std::optional(differentIdentity)),
              "strict unpackaged trust must reject a different executable file identity");
    }
    Check(!apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId(), std::nullopt),
          "strict unpackaged trust must reject a missing executable identity");
    Check(!apc::control::ExecutableIdentityFromPath(L"C:\\definitely-not-an-existing-executable.exe"),
          "invalid executable paths must fail closed without closing an invalid handle");
    Check(!apc::control::IsTrustedPeerProcess(0), "invalid peer identity must fail closed");
}

void TestStartStopHandleStability() {
    auto options = TestOptions(L"leaks", 1);
    const auto pipeName = options.PipeName;
    std::atomic_size_t acknowledgements = 0;
    options.AfterDeliveryCompleted = [&](apc::control::CorrelationId, bool acknowledged) noexcept {
        if (acknowledged) ++acknowledgements;
    };
    CommandLineControlServer server(std::move(options));
    const auto handler = [](apc::control::Request const&, std::stop_token, std::uint64_t) {
        return apc::control::Response{apc::control::ExitCode::Success, L"ok"};
    };

    for (std::uint64_t cycle = 0; cycle < 5; ++cycle) {
        server.Start(handler);
        (void)Exchange(pipeName, MakeRequest(100 + cycle));
        Check(WaitUntil([&] { return acknowledgements.load() >= cycle + 1; }, 1000),
              "warmup ACK must be observed before stopping the endpoint");
        server.Stop();
    }
    DWORD before = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "handle baseline must be readable");

    for (std::uint64_t cycle = 0; cycle < 100; ++cycle) {
        server.Start(handler);
        auto response = Exchange(pipeName, MakeRequest(1000 + cycle));
        Check(response && response->Code == apc::control::ExitCode::Success,
              "stress cycle must complete a production roundtrip");
        Check(WaitUntil([&] { return acknowledgements.load() >= cycle + 6; }, 1000),
              "stress ACK must be observed before stopping the endpoint");
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

    {
        auto options = TestOptions(L"cache-idle-prune", 1);
        options.AcknowledgedRecordLifetime = 30ms;
        Event cacheEmpty;
        options.AfterRequestCachePruned = [&](std::size_t records, std::size_t bytes) noexcept {
            if (records == 0 && bytes == 0) cacheEmpty.Signal();
        };
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"cached"};
        });
        auto response = Exchange(pipeName, MakeRequest(301));
        Check(response && response->Payload == L"cached", "cache-prune fixture must complete and acknowledge");
        Check(cacheEmpty.Wait(2000), "idle request records must be released when their TTL expires");
        server.Stop();
    }

    {
        auto options = TestOptions(L"unacknowledged-cache-idle-prune", 1);
        options.RequestRecordLifetime = 250ms;
        Event cacheEmpty;
        options.AfterRequestCachePruned = [&](std::size_t records, std::size_t bytes) noexcept {
            if (records == 0 && bytes == 0) cacheEmpty.Signal();
        };
        const auto pipeName = options.PipeName;
        CommandLineControlServer server(std::move(options));
        server.Start([](apc::control::Request const&, std::stop_token, std::uint64_t) {
            return apc::control::Response{apc::control::ExitCode::Success, L"unacknowledged"};
        });
        auto response = Exchange(pipeName, MakeRequest(302), false);
        Check(response && response->Payload == L"unacknowledged", "unacknowledged prune fixture must complete");
        Check(cacheEmpty.Wait(2000), "unacknowledged request records must be released when their TTL expires");
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
} // namespace

int RunCommandLineControlServerTests() {
    TestProductionRoundTripFragmentationAndRearm();
    TestDisconnectBeforeResponseRetriesExactlyOnce();
    TestDisconnectAfterResponseBeforeAckRetriesExactlyOnce();
    TestParallelDuplicatesAndCorrelationConflict();
    TestMalformedTimeoutOversizeAndRecovery();
    TestRequestStopCancelsLeasedBridgeWorkBeforeBridgeDrain();
    TestStopLifecycleAndRearmRetry();
    TestStartupSquattingAndSecurityDescriptor();
    TestStartStopHandleStability();
    TestMaximumRequestAndIdleCachePruning();
    TestStrictCommandSemantics();
    return g_failures;
}
