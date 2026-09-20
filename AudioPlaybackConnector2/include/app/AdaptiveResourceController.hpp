#pragma once

#include <app/AdaptiveResourcePolicy.hpp>
#include <app/AdaptiveActionRetryBackoff.hpp>
#include <app/AdaptiveScheduleState.hpp>
#include <app/AppModels.hpp>
#include <app/ResourcePressureMonitor.hpp>
#include <util/Logger.hpp>
#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

class TrayController;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Adaptive Resource Controller //////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Start, window messages and Stop run on UI. Monitor callbacks stage the pressure
// fence and dispatch weak work to UI; Snapshot alone is callable from other threads.
// The host retains this owner until after Stop has drained monitor callbacks.
class AdaptiveResourceController : public std::enable_shared_from_this<AdaptiveResourceController> {
public:
    using Dispatch = std::function<bool(std::function<void()>)>;
    explicit AdaptiveResourceController(util::LogSink log) : m_log(std::move(log)) {}
    ~AdaptiveResourceController();

    void Start(HWND hwnd,
               winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
               std::shared_ptr<TrayController> tray,
               Dispatch dispatch) noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool HandleMessage(UINT message, WPARAM value) noexcept;
    [[nodiscard]] apc::app::AppSnapshot::ResourceStatusSnapshot Snapshot() const;

private:
    void OnPressureChanged(ResourcePressureSnapshot snapshot);
    void Evaluate(bool userInteraction, std::wstring_view reason) noexcept;
    void ScheduleEvaluation(std::optional<AdaptiveResourcePolicy::TimePoint> reevaluateAt) noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    util::LogSink m_log;
    HWND m_hwnd = nullptr;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};
    // Tray stores only a weak callback to this owner, so these references cannot cycle.
    std::shared_ptr<TrayController> m_trayController;
    Dispatch m_dispatch;
    bool m_started = false;
    std::atomic_bool m_stopped = false;
    static constexpr UINT_PTR c_timerAdaptiveResources = 0x41504334;
    AdaptiveResourcePolicy m_policy;
    ResourcePressureValues m_pressure;
    std::unique_ptr<ResourcePressureMonitor> m_monitor;
    HPOWERNOTIFY m_powerNotification = nullptr;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_fallbackTimer{nullptr};
    AdaptiveActionRetryBackoff m_retry;
    AdaptiveScheduleState m_schedule;
    mutable std::mutex m_snapshotMutex;
    apc::app::AppSnapshot::ResourceStatusSnapshot m_snapshot;
    std::optional<AdaptiveResourcePolicy::TimePoint> m_lastObservedAt;
    std::uint64_t m_lastSequence = 0;
    std::uint64_t m_lastConstrainedSequence = 0;
};
