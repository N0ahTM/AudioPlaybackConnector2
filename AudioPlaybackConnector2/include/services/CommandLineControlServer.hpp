#pragma once

#include <control/CommandProtocol.hpp>

#include <wil/resource.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

class CommandLineControlServer {
public:
    using Handler = std::function<apc::control::Response(apc::control::Request const&, std::stop_token, std::uint64_t)>;

    ~CommandLineControlServer();

    void Start(Handler handler);
    void Stop() noexcept;

private:
    static constexpr std::size_t c_workerCount = 4;

    void WorkerLoop(std::stop_token stopToken) noexcept;
    bool ConnectClient(HANDLE pipe, std::stop_token stopToken) noexcept;
    void HandleClient(HANDLE pipe, std::stop_token stopToken) noexcept;

    std::atomic<bool> m_running = false;
    std::mutex m_lifecycleMutex;
    wil::unique_event m_stopEvent;
    std::wstring m_pipeName;
    Handler m_handler;
    std::vector<std::jthread> m_workers;
};
