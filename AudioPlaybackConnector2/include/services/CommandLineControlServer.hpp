#pragma once

#include <control/CommandProtocol.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class CommandLineControlServer {
public:
    using Handler = std::function<apc::control::Response(apc::control::Request const&)>;

    ~CommandLineControlServer();

    void Start(Handler handler);
    void Stop() noexcept;

private:
    void ListenLoop() noexcept;
    void HandleClient(HANDLE pipe) noexcept;

    std::atomic<bool> m_running = false;
    std::thread m_thread;
    Handler m_handler;
    std::wstring m_pipeName;
};
