#include <pch.h>

#include <services/CommandLineControlServer.hpp>

CommandLineControlServer::~CommandLineControlServer() {
    Stop();
}

void CommandLineControlServer::Start(Handler handler) {
    if (m_running.exchange(true)) return;

    m_handler = std::move(handler);
    m_pipeName = apc::control::PipeName();
    m_thread = std::thread([this]() noexcept { ListenLoop(); });
}

void CommandLineControlServer::Stop() noexcept {
    if (!m_running.exchange(false)) return;

    if (!m_pipeName.empty()) {
        wil::unique_hfile unblocker(
            CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    }

    if (m_thread.joinable()) {
        try {
            m_thread.join();
        } catch (...) {
        }
    }

    m_handler = nullptr;
}

void CommandLineControlServer::ListenLoop() noexcept {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (...) {
        util::DebugTraceUnknownException(L"[CommandLineControlServer] init_apartment failed");
    }
    auto apartmentGuard = wil::scope_exit([&]() noexcept {
        if (apartmentInitialized) {
            winrt::uninit_apartment();
        }
    });

    while (m_running.load()) {
        wil::unique_hfile pipe(
            CreateNamedPipeW(m_pipeName.c_str(),
                             PIPE_ACCESS_DUPLEX,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             PIPE_UNLIMITED_INSTANCES,
                             apc::control::c_pipeBufferBytes,
                             apc::control::c_pipeBufferBytes,
                             0,
                             nullptr));
        if (!pipe) {
            util::DebugTraceException(L"[CommandLineControlServer] CreateNamedPipeW failed",
                                      winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError())));
            Sleep(250);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            continue;
        }

        if (m_running.load()) {
            HandleClient(pipe.get());
        }

        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());
    }
}

void CommandLineControlServer::HandleClient(HANDLE pipe) noexcept {
    apc::control::Response response{apc::control::ExitCode::InvalidRequest, L""};
    try {
        apc::control::Request request;
        if (apc::control::ReadRequest(pipe, request)) {
            auto handler = m_handler;
            if (handler) {
                response = handler(request);
            } else {
                response = {apc::control::ExitCode::Unavailable, L""};
            }
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

    (void)apc::control::WriteResponse(pipe, response);
}
