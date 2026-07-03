#pragma once

#include <control/CommandProtocol.hpp>

#include <wil/resource.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
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
    void DispatchClient(wil::unique_hfile pipe) noexcept;
    void HandleClient(HANDLE pipe) noexcept;

    std::atomic<bool> m_running = false;
    std::atomic<uint32_t> m_activeClients = 0;
    std::thread m_thread;
    std::mutex m_handlerMutex;
    std::mutex m_activeClientsMutex;
    std::condition_variable m_activeClientsCv;
    Handler m_handler;
    std::wstring m_pipeName;
};
