#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Reconnect Controller //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ReconnectController {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Data Structures ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct TimerToken {
        std::wstring DeviceId;
        std::uint64_t GlobalGeneration = 0;
        std::uint64_t DeviceGeneration = 0;
        std::size_t Attempt = 0;
    };

    struct ScheduleDecision {
        bool ShouldSchedule = false;
        bool NotifyFailed = false;
        bool AttemptCompleted = false;
        std::size_t Attempt = 0;
        std::size_t MaxAttempts = 0;
        std::chrono::seconds Delay{};
        TimerToken Token;
    };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static std::chrono::milliseconds ReconnectCloseCooldown();

    void AllowReconnects();
    void CancelPendingReconnects();
    void ClearTracking();
    void BeginManualOperation(std::wstring_view deviceId);
    void CancelDevice(std::wstring_view deviceId);
    void SetPolicyEnabled(std::wstring_view deviceId, bool enabled);
    void CompleteConnectionSucceeded(std::wstring_view deviceId);
    [[nodiscard]] bool IsCancelled(std::wstring_view deviceId) const;
    [[nodiscard]] bool AllReconnectsCancelled() const;
    [[nodiscard]] std::size_t Attempts(std::wstring_view deviceId) const;
    [[nodiscard]] bool HasPendingTimer(std::wstring_view deviceId) const;
    [[nodiscard]] bool HasAttemptInProgress(std::wstring_view deviceId) const;
    [[nodiscard]] bool HasPendingTimers() const;
    [[nodiscard]] std::vector<std::wstring> PendingDeviceIds() const;

    [[nodiscard]] ScheduleDecision PrepareSchedule(std::wstring_view deviceId, bool blocked);
    [[nodiscard]] bool ClaimTimer(TimerToken const& token);
    [[nodiscard]] bool RetireTimer(TimerToken const& token);
    [[nodiscard]] bool AbortTimerOrAttempt(TimerToken const& token);
    [[nodiscard]] ScheduleDecision DeferTimer(TimerToken const& token);
    [[nodiscard]] ScheduleDecision CompleteAttemptFailed(TimerToken const& token);
    void CompleteAttemptSucceeded(TimerToken const& token);
    [[nodiscard]] bool HandleTimerCreateFailed(TimerToken const& token);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Data Structures ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct DeviceState {
        std::size_t CompletedAttempts = 0;
        std::uint64_t Generation = 0;
        bool TimerPending = false;
        bool AttemptInProgress = false;
        bool UserCancelled = false;
        bool PolicyEnabled = true;
        bool FailureNotified = false;
    };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::unordered_map<std::wstring, DeviceState> m_states;
    std::uint64_t m_globalGeneration = 0;
    bool m_allReconnectsCancelled = false;
};
