#pragma once

#include <util/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Lifecycle methods run on the host's UI context. Timer delivery and completions
// use independent shared state; they never access the coordinator facade.
class PowerTransitionCoordinator {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Types /////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ResumeReconnectCompleted = std::function<void(std::vector<std::wstring>)>;
    using ResumeReconnectCallback =
        std::function<void(std::vector<std::wstring>, std::uint64_t, ResumeReconnectCompleted)>;
    using Tick = std::function<bool()>; // false stops the periodic schedule
    using CancelTimer = std::move_only_function<void()>;
    // Returns an empty cancellation handle on scheduling failure. The scheduler
    // must not call Tick inline. Cancellation is noexcept and permits reentrancy
    // from Tick; already admitted delivery may finish using its own shared state.
    using Scheduler = std::move_only_function<CancelTimer(std::chrono::milliseconds, Tick)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    explicit PowerTransitionCoordinator(std::atomic<bool>& exiting, Scheduler scheduler = {}, util::LogSink log = {});
    ~PowerTransitionCoordinator();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Cancel() noexcept;
    void HandleSuspend(std::function<void()> flushSettings,
                       std::function<std::vector<std::wstring>()> suspendDevices) noexcept;
    void HandleResume(std::function<void()> resumeDevices, ResumeReconnectCallback reconnectAfterDelay) noexcept;
    void NotifyDeviceConnected(std::wstring_view deviceId) noexcept;
    [[nodiscard]] bool IsResumeReconnectGenerationCurrent(std::uint64_t generation) const noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct ResumeState;
    void CancelResumeReconnectTimer() noexcept;
    [[nodiscard]] static bool DeliverResumeReconnect(std::shared_ptr<ResumeState> const& state,
                                                     std::uint64_t generation) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::atomic<bool>& m_exiting;
    bool m_powerSuspended = false;
    std::shared_ptr<ResumeState> m_resumeState;
    std::chrono::steady_clock::time_point m_lastResumeHandledAt{};
    Scheduler m_schedule;
    CancelTimer m_cancelTimer;
};
