#pragma once

#include <chrono>
#include <unordered_map>
#include <unordered_set>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Reconnect Controller //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class ReconnectController {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Data Structures ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct ScheduleDecision {
        bool ShouldSchedule = false;
        bool NotifyFailed = false;
        std::size_t Attempt = 0;
        std::size_t MaxAttempts = 0;
        std::chrono::seconds Delay{};
    };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    static std::chrono::milliseconds ReconnectCloseCooldown();

    void AllowReconnects();
    void CancelPendingReconnects();
    void ClearTracking();
    void BeginManualReconnect(winrt::hstring const& deviceId);
    void ClearAttempts(winrt::hstring const& deviceId);
    void MarkUserCancelled(winrt::hstring const& deviceId);
    void ClearUserCancellationIfNoTimer(winrt::hstring const& deviceId);
    void ClearCancelled(winrt::hstring const& deviceId);
    [[nodiscard]] bool IsCancelled(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool AllReconnectsCancelled() const;
    [[nodiscard]] std::size_t Attempts(winrt::hstring const& deviceId) const;

    [[nodiscard]] ScheduleDecision PrepareSchedule(winrt::hstring const& deviceId, bool blocked);
    void StartTimer(winrt::hstring const& deviceId);
    [[nodiscard]] bool ShouldSkipTimer(winrt::hstring const& deviceId, bool shutdownForProcessExit) const;
    void CompleteTimer(winrt::hstring const& deviceId);
    void HandleTimerCreateFailed(winrt::hstring const& deviceId);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::unordered_set<winrt::hstring> m_cancelledReconnectIds;
    std::unordered_map<winrt::hstring, std::size_t> m_reconnectTimerCounts;
    std::unordered_map<winrt::hstring, std::size_t> m_reconnectAttempts;
    bool m_allReconnectsCancelled = false;
};
