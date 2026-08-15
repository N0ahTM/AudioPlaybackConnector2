#include <pch.h>

#include <app/PowerTransitionCoordinator.hpp>

#include <core/DeviceManager.hpp>

namespace {
constexpr std::chrono::seconds c_resumeReconnectDelay{10};
constexpr std::chrono::seconds c_duplicateResumeWindow{2};
constexpr unsigned int c_maxResumeReconnectAttempts = 6;
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

PowerTransitionCoordinator::PowerTransitionCoordinator(std::atomic<bool>& exiting)
    : m_exiting(exiting), m_resumeState(std::make_shared<ResumeState>()) {}

PowerTransitionCoordinator::~PowerTransitionCoordinator() {
    Cancel();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::Cancel() noexcept {
    auto state = m_resumeState;
    if (state) {
        std::scoped_lock lock(state->Mutex);
        state->Cancelled = true;
        state->Attempts.Clear();
        state->Reconnect = nullptr;
        state->DeliveryInFlight = false;
        ++state->Generation;
    }
    CancelResumeReconnectTimer();
}

void PowerTransitionCoordinator::HandleSuspend(std::function<void()> flushSettings,
                                               std::shared_ptr<DeviceManager> deviceManager) noexcept {
    try {
        if (m_exiting.load() || m_powerSuspended) return;
        m_powerSuspended = true;
        DebugTrace(L"[PowerTransitionCoordinator] Power suspend detected");

        if (auto state = m_resumeState) {
            std::scoped_lock lock(state->Mutex);
            state->Cancelled = true;
            state->Reconnect = nullptr;
            state->DeliveryInFlight = false;
            ++state->Generation;
        }
        CancelResumeReconnectTimer();

        std::vector<std::wstring> activeDeviceIds;
        if (deviceManager) {
            try {
                auto connected = deviceManager->GetConnectedDevices();
                activeDeviceIds.reserve(connected.size());
                for (auto const& connection : connected) {
                    if (!connection.Id.empty()) activeDeviceIds.push_back(connection.Id);
                }
            } catch (...) {
                DebugTrace(L"[PowerTransitionCoordinator] Failed to capture active devices before suspend");
            }
        }

        if (auto state = m_resumeState) {
            std::scoped_lock lock(state->Mutex);
            state->Cancelled = false;
            ++state->Generation;
            state->Reconnect = nullptr;
            state->DeliveryInFlight = false;
            state->Attempts.BeginCycle(std::move(activeDeviceIds));
        }

        if (flushSettings) flushSettings();
        if (deviceManager) deviceManager->SuspendForPowerTransition();
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] HandleSuspend ERROR: ignored exception");
    }
}

void PowerTransitionCoordinator::HandleResume(std::shared_ptr<DeviceManager> deviceManager,
                                              ResumeReconnectCallback reconnectAfterDelay) noexcept {
    try {
        if (m_exiting.load()) return;

        const auto now = std::chrono::steady_clock::now();
        if (!m_powerSuspended && m_lastResumeHandledAt != std::chrono::steady_clock::time_point{} &&
            now - m_lastResumeHandledAt < c_duplicateResumeWindow) {
            DebugTrace(L"[PowerTransitionCoordinator] Duplicate power resume ignored");
            return;
        }
        m_lastResumeHandledAt = now;

        const bool matchedSuspend = m_powerSuspended;
        if (matchedSuspend) {
            m_powerSuspended = false;
            DebugTrace(L"[PowerTransitionCoordinator] Power resume detected");
        } else {
            DebugTrace(L"[PowerTransitionCoordinator] Power resume detected without prior suspend; running recovery");
        }

        if (deviceManager) deviceManager->ResumeAfterPowerTransition();
        if (!matchedSuspend) return;

        CancelResumeReconnectTimer();

        auto state = m_resumeState;
        if (!state) return;
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(state->Mutex);
            if (state->Cancelled || state->Attempts.Empty()) return;
            generation = state->Generation;
            state->Reconnect = std::move(reconnectAfterDelay);
            state->DeliveryInFlight = false;
        }

        try {
            m_resumeReconnectTimer = winrt::Windows::System::Threading::ThreadPoolTimer::CreatePeriodicTimer(
                [state, generation](auto const& timer) noexcept {
                    if (DeliverResumeReconnect(state, generation) != DeliveryResult::Stop) return;
                    try {
                        timer.Cancel();
                    } catch (...) {
                    }
                },
                c_resumeReconnectDelay);
        } catch (...) {
            DebugTrace(L"[PowerTransitionCoordinator] WinRT resume timer unavailable; using native timer");
            m_nativeResumeReconnectTimer.reset(
                CreateThreadpoolTimer(NativeResumeReconnectTimerCallback, this, nullptr));
            if (m_nativeResumeReconnectTimer) {
                LARGE_INTEGER relative{};
                relative.QuadPart = -static_cast<LONGLONG>(c_resumeReconnectDelay.count()) * 10'000'000LL;
                FILETIME dueTime{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
                SetThreadpoolTimer(
                    m_nativeResumeReconnectTimer.get(),
                    &dueTime,
                    static_cast<DWORD>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(c_resumeReconnectDelay).count()),
                    0);
            } else {
                DebugTrace(L"[PowerTransitionCoordinator] Resume reconnect timer unavailable");
            }
        }
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] HandleResume ERROR: ignored exception");
    }
}

void PowerTransitionCoordinator::NotifyDeviceConnected(std::wstring_view deviceId) noexcept {
    if (deviceId.empty()) return;
    auto state = m_resumeState;
    if (!state) return;

    bool completed = false;
    {
        std::scoped_lock lock(state->Mutex);
        static_cast<void>(state->Attempts.Acknowledge(deviceId));
        completed = state->Attempts.Empty();
    }
    if (completed) CancelResumeReconnectTimer();
}

bool PowerTransitionCoordinator::IsResumeReconnectGenerationCurrent(std::uint64_t generation) const noexcept {
    auto state = m_resumeState;
    if (!state) return false;
    std::scoped_lock lock(state->Mutex);
    return !state->Cancelled && state->Generation == generation && !state->Attempts.Empty();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::CancelResumeReconnectTimer() noexcept {
    auto timer = std::exchange(m_resumeReconnectTimer, nullptr);
    if (timer) {
        try {
            timer.Cancel();
        } catch (...) {
        }
    }
    auto nativeTimer = std::move(m_nativeResumeReconnectTimer);
    if (nativeTimer) {
        SetThreadpoolTimer(nativeTimer.get(), nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(nativeTimer.get(), TRUE);
    }
    nativeTimer.reset();
}

PowerTransitionCoordinator::DeliveryResult
PowerTransitionCoordinator::DeliverResumeReconnect(std::shared_ptr<ResumeState> const& state,
                                                   std::uint64_t generation,
                                                   PTP_CALLBACK_INSTANCE callbackInstance) noexcept {
    if (!state) return DeliveryResult::Stop;

    std::vector<std::wstring> deviceIds;
    ResumeReconnectCallback reconnect;
    {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation || state->Attempts.Empty()) {
            return DeliveryResult::Stop;
        }
        if (state->DeliveryInFlight) return DeliveryResult::Continue;

        auto selection = state->Attempts.SelectEligible(c_maxResumeReconnectAttempts);
        for (auto const& id : selection.Exhausted) {
            DebugTrace(L"[PowerTransitionCoordinator] Resume reconnect retry limit reached for {0}", id);
        }
        if (selection.Eligible.empty()) return DeliveryResult::Stop;

        reconnect = state->Reconnect;
        if (!reconnect) return DeliveryResult::Continue;
        deviceIds = std::move(selection.Eligible);
        state->DeliveryInFlight = true;
    }

    auto completed = [state, generation](std::vector<std::wstring> attemptedIds) noexcept {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation) return;
        state->Attempts.RecordAttempts(attemptedIds);
        state->DeliveryInFlight = false;
    };
    if (callbackInstance) DisassociateCurrentThreadFromCallback(callbackInstance);
    try {
        reconnect(std::move(deviceIds), generation, completed);
    } catch (...) {
        completed({});
        DebugTrace(L"[PowerTransitionCoordinator] Delayed resume reconnect callback failed");
    }
    return callbackInstance ? DeliveryResult::CallbackDisassociated : DeliveryResult::Continue;
}

void CALLBACK PowerTransitionCoordinator::NativeResumeReconnectTimerCallback(PTP_CALLBACK_INSTANCE callbackInstance,
                                                                             void* context,
                                                                             PTP_TIMER timer) noexcept {
    auto self = static_cast<PowerTransitionCoordinator*>(context);
    if (!self || self->m_exiting.load()) return;
    auto state = self->m_resumeState;
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(state->Mutex);
        generation = state->Generation;
    }
    auto const result = DeliverResumeReconnect(state, generation, callbackInstance);
    if (result == DeliveryResult::Stop) SetThreadpoolTimer(timer, nullptr, 0, 0);
}
