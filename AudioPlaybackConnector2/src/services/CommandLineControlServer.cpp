#include <pch.h>

#include <services/CommandLineControlServer.hpp>

namespace {
constexpr DWORD c_retryDelayMs = 250;
constexpr DWORD c_requestIoTimeoutMs = 5000;
constexpr DWORD c_responseIoTimeoutMs = 5000;
constexpr DWORD c_handlerTimeoutMs = 30000;
} // namespace

CommandLineControlServer::~CommandLineControlServer() {
    Stop();
}

void CommandLineControlServer::Start(Handler handler) {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    if (m_running.load()) return;

    m_stopEvent.create(wil::EventOptions::ManualReset);
    m_pipeName = apc::control::PipeName();
    m_handler = std::move(handler);
    m_running = true;

    try {
        m_workers.reserve(c_workerCount);
        for (std::size_t index = 0; index < c_workerCount; ++index) {
            m_workers.emplace_back([this](std::stop_token stopToken) noexcept { WorkerLoop(stopToken); });
        }
    } catch (...) {
        m_running = false;
        m_stopEvent.SetEvent();
        for (auto& worker : m_workers) {
            worker.request_stop();
        }
        m_workers.clear();
        m_handler = nullptr;
        m_pipeName.clear();
        m_stopEvent.reset();
        throw;
    }
}

void CommandLineControlServer::Stop() noexcept {
    try {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (!m_running.exchange(false)) return;

        if (m_stopEvent) m_stopEvent.SetEvent();
        for (auto& worker : m_workers) {
            worker.request_stop();
        }
        m_workers.clear();
        m_handler = nullptr;
        m_pipeName.clear();
        m_stopEvent.reset();
    } catch (...) {
        util::DebugTraceUnknownException(L"[CommandLineControlServer] Stop ignored exception");
    }
}

void CommandLineControlServer::WorkerLoop(std::stop_token stopToken) noexcept {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (...) {
        util::DebugTraceUnknownException(L"[CommandLineControlServer] worker init_apartment failed");
    }
    auto apartmentGuard = wil::scope_exit([&]() noexcept {
        if (apartmentInitialized) winrt::uninit_apartment();
    });

    while (m_running.load() && !stopToken.stop_requested()) {
        wil::unique_hfile pipe(
            CreateNamedPipeW(m_pipeName.c_str(),
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             static_cast<DWORD>(c_workerCount),
                             apc::control::c_pipeBufferBytes,
                             apc::control::c_pipeBufferBytes,
                             0,
                             nullptr));
        if (!pipe) {
            util::DebugTraceException(L"[CommandLineControlServer] CreateNamedPipeW failed",
                                      winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError())));
            if (WaitForSingleObject(m_stopEvent.get(), c_retryDelayMs) == WAIT_OBJECT_0) break;
            continue;
        }

        if (!ConnectClient(pipe.get(), stopToken)) continue;
        HandleClient(pipe.get(), stopToken);
        DisconnectNamedPipe(pipe.get());
    }
}

bool CommandLineControlServer::ConnectClient(HANDLE pipe, std::stop_token stopToken) noexcept {
    wil::unique_event completed;
    try {
        completed.create(wil::EventOptions::ManualReset);
    } catch (...) {
        util::DebugTraceUnknownException(L"[CommandLineControlServer] connect event creation failed");
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = completed.get();
    if (ConnectNamedPipe(pipe, &overlapped)) return true;

    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;

    HANDLE handles[]{completed.get(), m_stopEvent.get()};
    const auto waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0 && !stopToken.stop_requested()) {
        DWORD transferred = 0;
        return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    }

    CancelIoEx(pipe, &overlapped);
    DWORD transferred = 0;
    (void)GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    return false;
}

void CommandLineControlServer::HandleClient(HANDLE pipe, std::stop_token stopToken) noexcept {
    apc::control::Response response{apc::control::ExitCode::InvalidRequest, L""};
    try {
        apc::control::Request request;
        const auto readStatus = apc::control::ReadRequest(
            pipe, request, m_stopEvent.get(), apc::control::DeadlineAfter(c_requestIoTimeoutMs));
        if (readStatus == apc::control::IoStatus::Success) {
            if (m_handler) {
                response = m_handler(request, stopToken, apc::control::DeadlineAfter(c_handlerTimeoutMs));
            } else {
                response = {apc::control::ExitCode::Unavailable, L""};
            }
        } else if (readStatus == apc::control::IoStatus::Cancelled) {
            return;
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[CommandLineControlServer] HandleClient failed", ex);
        response = {apc::control::ExitCode::OperationFailed, ex.message().c_str()};
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[CommandLineControlServer] HandleClient failed", ex);
        response = {apc::control::ExitCode::OperationFailed, util::Utf8ToUtf16(ex.what())};
    } catch (...) {
        util::DebugTraceUnknownException(L"[CommandLineControlServer] HandleClient failed");
        response = {apc::control::ExitCode::OperationFailed, L""};
    }

    if (stopToken.stop_requested()) return;
    (void)apc::control::WriteResponse(
        pipe, response, m_stopEvent.get(), apc::control::DeadlineAfter(c_responseIoTimeoutMs));
}
