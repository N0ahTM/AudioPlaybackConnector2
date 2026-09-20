#include <pch.h>
#include <app/AdaptiveResourceController.hpp>
#include <services/TrayController.hpp>
#include <app/ResourcePressureState.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Policy Projection /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {
constexpr auto c_resourcePressureSnapshotMaximumAge = std::chrono::seconds{75};
apc::app::AppSnapshot::ResourceStatusSnapshot::Residency ToAppResidency(ResidencyPolicy value) noexcept {
    using Residency = apc::app::AppSnapshot::ResourceStatusSnapshot::Residency;
    switch (value) {
        case ResidencyPolicy::Cold: return Residency::Cold;
        case ResidencyPolicy::Warm: return Residency::Warm;
        case ResidencyPolicy::Hot: return Residency::Hot;
    }
    return Residency::Warm;
}

apc::app::AppSnapshot::ResourceStatusSnapshot::MemoryPressure ToAppMemoryPressure(MemoryPressureState value) noexcept {
    using MemoryPressure = apc::app::AppSnapshot::ResourceStatusSnapshot::MemoryPressure;
    switch (value) {
        case MemoryPressureState::Unknown: return MemoryPressure::Unknown;
        case MemoryPressureState::Low: return MemoryPressure::Low;
        case MemoryPressureState::Neutral: return MemoryPressure::Neutral;
        case MemoryPressureState::High: return MemoryPressure::High;
    }
    return MemoryPressure::Unknown;
}

apc::app::AppSnapshot::ResourceStatusSnapshot::UserActivity ToAppUserActivity(UserActivityState value) noexcept {
    using UserActivity = apc::app::AppSnapshot::ResourceStatusSnapshot::UserActivity;
    switch (value) {
        case UserActivityState::Unknown: return UserActivity::Unknown;
        case UserActivityState::Available: return UserActivity::Available;
        case UserActivityState::NotPresent: return UserActivity::NotPresent;
        case UserActivityState::Busy: return UserActivity::Busy;
        case UserActivityState::Fullscreen: return UserActivity::Fullscreen;
        case UserActivityState::Presentation: return UserActivity::Presentation;
        case UserActivityState::QuietTime: return UserActivity::QuietTime;
        case UserActivityState::ImmersiveApp: return UserActivity::ImmersiveApp;
    }
    return UserActivity::Unknown;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Lifecycle and Window Messages /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AdaptiveResourceController::~AdaptiveResourceController() {
    Stop();
}

void AdaptiveResourceController::Stop() noexcept {
    if (m_stopped.exchange(true)) return;
    if (m_monitor) {
        m_monitor->Stop();
        m_monitor.reset();
    }
    if (auto notification = std::exchange(m_powerNotification, nullptr)) {
        if (!UnregisterPowerSettingNotification(notification)) {
            m_log.Trace(L"[Resources] Failed to unregister battery-saver notification: {0}", GetLastError());
        }
    }
    if (m_fallbackTimer) {
        try {
            m_fallbackTimer.Stop();
        } catch (...) { /* Stop still invalidates the generation and closes admission. */
        }
        m_fallbackTimer = nullptr;
    }
    static_cast<void>(m_schedule.Supersede());
    if (m_hwnd) KillTimer(m_hwnd, c_timerAdaptiveResources);
    if (m_trayController) m_trayController->SetResourceStateChangedCallback({});
    m_trayController.reset();
    m_dispatch = {};
    m_dispatcherQueue = nullptr;
}

bool AdaptiveResourceController::HandleMessage(UINT message, WPARAM value) noexcept {
    if (m_stopped.load()) return false;
    if (message == WM_TIMER && value == c_timerAdaptiveResources) {
        if (m_schedule.ConsumeWin32IfDue(AdaptiveResourcePolicy::Clock::now())) {
            KillTimer(m_hwnd, c_timerAdaptiveResources);
            Evaluate(false, L"adaptive-deadline");
        }
        return true;
    }
    if (message == WM_POWERBROADCAST && m_monitor &&
        (value == PBT_APMRESUMEAUTOMATIC || value == PBT_APMRESUMESUSPEND || value == PBT_POWERSETTINGCHANGE)) {
        static_cast<void>(m_monitor->RequestProbe());
    }
    return false; // Power transitions also belong to the host's suspend/resume lifecycle.
}

apc::app::AppSnapshot::ResourceStatusSnapshot AdaptiveResourceController::Snapshot() const {
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot;
}

void AdaptiveResourceController::Start(HWND hwnd,
                                       winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                       std::shared_ptr<TrayController> tray,
                                       Dispatch dispatch) noexcept {
    if (m_started || m_stopped.load()) return;
    m_started = true;
    m_hwnd = hwnd;
    m_dispatcherQueue = std::move(dispatcher);
    m_trayController = std::move(tray);
    m_dispatch = std::move(dispatch);
    if (!m_trayController) return;
    try {
        auto weak = weak_from_this();
        m_trayController->SetResourceStateChangedCallback([weak](bool userInteraction) {
            if (auto self = weak.lock(); self && !self->m_stopped.load()) {
                self->Evaluate(userInteraction, L"tray-ui-state");
            }
        });

        m_monitor = std::make_unique<ResourcePressureMonitor>([weak](ResourcePressureSnapshot const& value) {
            if (auto self = weak.lock(); self && !self->m_stopped.load()) {
                if (value.Values.IsBackgroundConstrained()) {
                    std::scoped_lock authorizationLock(self->m_snapshotMutex);
                    self->m_lastConstrainedSequence = std::max(self->m_lastConstrainedSequence, value.Sequence);
                }
                self->OnPressureChanged(value);
            }
        });

        if (!m_monitor->Start()) {
            m_monitor.reset();
            m_log.Trace(L"[App] Resource-pressure monitor unavailable; speculative preloading remains disabled");
        } else if (!m_stopped.load()) {
            m_powerNotification =
                RegisterPowerSettingNotification(m_hwnd, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (!m_powerNotification) {
                m_log.Trace(L"[App] Battery-saver notification registration unavailable; polling fallback remains "
                            L"active");
            }
        }
    } catch (...) {
        m_monitor.reset();
        OutputDebugStringW(L"[AudioPlaybackConnector2] Resource-pressure monitor initialization failed\n");
    }
    Evaluate(false, L"adaptive-startup");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Pressure and Resource Evaluation //////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void AdaptiveResourceController::OnPressureChanged(ResourcePressureSnapshot snapshot) {
    auto weak = weak_from_this();
    static_cast<void>(m_dispatch([weak, snapshot = std::move(snapshot)]() mutable {
        auto self = weak.lock();
        if (!self || self->m_stopped.load() || snapshot.Sequence <= self->m_lastSequence) return;

        self->m_lastSequence = snapshot.Sequence;
        self->m_pressure = snapshot.Values;
        self->m_lastObservedAt = snapshot.ObservedAt;
        self->Evaluate(false, L"resource-pressure-change");
    }));
}

void AdaptiveResourceController::Evaluate(bool userInteraction, std::wstring_view reason) noexcept {
    if (userInteraction) m_retry.Reset();

    try {
        if (m_stopped.load() || !m_trayController || !m_hwnd || !IsWindow(m_hwnd)) return;

        // Native picker work may reenter host teardown; keep its owner alive until return.
        auto tray = m_trayController;
        auto const now = AdaptiveResourcePolicy::Clock::now();
        const bool snapshotFresh =
            IsResourcePressureSnapshotFresh(m_lastObservedAt, now, c_resourcePressureSnapshotMaximumAge);
        auto const pressureValues = snapshotFresh ? m_pressure : ResourcePressureValues{};
        const bool energySaver = pressureValues.EnergySaver == true;
        const bool backgroundConstrained = pressureValues.IsBackgroundConstrained();
        bool positiveAuthorizationCurrent;
        {
            std::scoped_lock authorizationLock(m_snapshotMutex);
            positiveAuthorizationCurrent =
                IsPositiveResourceAuthorizationCurrent(m_lastSequence, m_lastConstrainedSequence);
        }
        // This capture admits the evaluation. Later pressure observations schedule
        // another UI evaluation; never hold the fence mutex across picker calls.
        AdaptiveResourcePolicyInput input{
            .MemoryPressure = pressureValues.IsMemoryPressure(),
            .PreloadAllowed = snapshotFresh && positiveAuthorizationCurrent && pressureValues.CanPreload(),
            .FullscreenOrPresentation = backgroundConstrained && !pressureValues.IsMemoryPressure() && !energySaver,
            .EnergySaver = energySaver,
            .UiVisible = tray->IsDevicePickerVisibleOrTransitioning(),
            .UiPinned = false,
            .UserInteraction = userInteraction,
            .UiResourcesLoaded = tray->IsDevicePickerLoaded(),
            .UiResourcesInitialized = tray->IsDevicePickerPreloadInitialized(),
        };

        auto decision = m_policy.Evaluate(input, now);
        if (decision.ResidencyChanged || decision.BackgroundResidencyChanged ||
            decision.Action != AdaptiveResourceAction::None) {
            m_log.Trace(L"[App] Adaptive resources reason={0} residency={1} background={2} action={3} memory={4} "
                        L"activity={5} energySaver={6}",
                        reason,
                        static_cast<int>(decision.Residency),
                        static_cast<int>(decision.BackgroundResidency),
                        static_cast<int>(decision.Action),
                        static_cast<int>(pressureValues.Memory),
                        static_cast<int>(pressureValues.UserActivity),
                        energySaver);
        }

        switch (decision.Action) {
            case AdaptiveResourceAction::PreloadUi: tray->PreloadDevicePicker(); break;
            case AdaptiveResourceAction::ReleaseUi: tray->ReleaseDevicePicker(); break;
            case AdaptiveResourceAction::None: break;
        }

        // Picker calls may pump UI messages and reenter Stop. Do not publish or
        // schedule another evaluation after that teardown boundary.
        if (m_stopped.load()) return;
        auto reevaluateAt = decision.ReevaluateAt;
        if (snapshotFresh && m_lastObservedAt) {
            auto const snapshotExpiry = *m_lastObservedAt + c_resourcePressureSnapshotMaximumAge;
            if (!reevaluateAt || snapshotExpiry < *reevaluateAt) reevaluateAt = snapshotExpiry;
        }
        const bool actionSucceeded =
            decision.Action == AdaptiveResourceAction::None ||
            (decision.Action == AdaptiveResourceAction::PreloadUi && tray->IsDevicePickerPreloadInitialized()) ||
            (decision.Action == AdaptiveResourceAction::ReleaseUi && !tray->IsDevicePickerLoaded());
        if (!actionSucceeded) {
            auto const retryAt = AdaptiveResourcePolicy::Clock::now() + m_retry.RecordFailure();
            if (!reevaluateAt || retryAt < *reevaluateAt) reevaluateAt = retryAt;
        } else {
            m_retry.Reset();
        }
        apc::app::AppSnapshot::ResourceStatusSnapshot status = {
            .Evaluated = true,
            .ForegroundResidency = ToAppResidency(decision.Residency),
            .BackgroundResidency = ToAppResidency(decision.BackgroundResidency),
            .SnapshotFresh = snapshotFresh,
            .PositiveAuthorizationCurrent = positiveAuthorizationCurrent,
            .PreloadAllowed = input.PreloadAllowed,
            .UiResourcesLoaded = tray->IsDevicePickerLoaded(),
            .UiResourcesInitialized = tray->IsDevicePickerPreloadInitialized(),
            .Memory = ToAppMemoryPressure(pressureValues.Memory),
            .Activity = ToAppUserActivity(pressureValues.UserActivity),
            .EnergySaver = pressureValues.EnergySaver,
        };
        {
            std::scoped_lock authorizationLock(m_snapshotMutex);
            status.PositiveAuthorizationCurrent =
                IsPositiveResourceAuthorizationCurrent(m_lastSequence, m_lastConstrainedSequence);
            status.PreloadAllowed = input.PreloadAllowed && status.PositiveAuthorizationCurrent;
            m_snapshot = status;
        }
        ScheduleEvaluation(reevaluateAt);
    } catch (...) {
        OutputDebugStringW(L"[AudioPlaybackConnector2] Adaptive resource evaluation failed\n");
        auto const retryAt = AdaptiveResourcePolicy::Clock::now() + m_retry.RecordFailure();
        ScheduleEvaluation(retryAt);
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Evaluation Schedule ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void AdaptiveResourceController::ScheduleEvaluation(
    std::optional<AdaptiveResourcePolicy::TimePoint> reevaluateAt) noexcept {
    auto const scheduleGeneration = m_schedule.Supersede();
    if (m_fallbackTimer) {
        try {
            m_fallbackTimer.Stop();
        } catch (...) {
        }
        m_fallbackTimer = nullptr;
    }
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        static_cast<void>(m_schedule.Consume(scheduleGeneration));
        return;
    }
    KillTimer(m_hwnd, c_timerAdaptiveResources);
    if (!reevaluateAt || m_stopped.load()) {
        static_cast<void>(m_schedule.Consume(scheduleGeneration));
        return;
    }

    const auto now = AdaptiveResourcePolicy::Clock::now();
    auto remaining = *reevaluateAt > now ? *reevaluateAt - now : AdaptiveResourcePolicy::Clock::duration::zero();
    auto delay = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    delay = std::clamp<std::int64_t>(delay, 1, std::numeric_limits<UINT>::max());
    if (SetTimer(m_hwnd, c_timerAdaptiveResources, static_cast<UINT>(delay), nullptr)) {
        static_cast<void>(m_schedule.SetWin32NotBefore(scheduleGeneration, now + std::chrono::milliseconds{delay}));
        return;
    }

    OutputDebugStringW(L"[AudioPlaybackConnector2] Win32 adaptive timer unavailable; using dispatcher fallback\n");
    try {
        if (!m_dispatcherQueue) return;
        auto timer = m_dispatcherQueue.CreateTimer();
        timer.Interval(std::chrono::milliseconds{delay});
        timer.IsRepeating(false);
        auto weak = weak_from_this();
        timer.Tick([weak, scheduleGeneration](auto const& sender, auto const&) noexcept {
            try {
                sender.Stop();
            } catch (...) {
            }
            if (auto self = weak.lock();
                self && !self->m_stopped.load() && self->m_schedule.Consume(scheduleGeneration)) {
                self->m_fallbackTimer = nullptr;
                self->Evaluate(false, L"adaptive-dispatcher-deadline");
            }
        });
        if (m_stopped.load()) return;
        m_fallbackTimer = timer;
        timer.Start();
    } catch (...) {
        m_fallbackTimer = nullptr;
        OutputDebugStringW(L"[AudioPlaybackConnector2] Failed to schedule adaptive resource fallback timer\n");
    }
}
