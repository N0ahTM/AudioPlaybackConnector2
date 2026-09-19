#include <app/PowerTransitionCoordinator.hpp>
#include <app/ResumeReconnectAttemptState.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>

#include <windows.h>
#include <wil/resource.h>

#include <mutex>
#include <utility>

namespace {
/*------------------------------------------------------------------------------------------------------------*/
/*//////// Native Timer //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

constexpr std::chrono::seconds c_resumeReconnectDelay{10};
constexpr std::chrono::seconds c_duplicateResumeWindow{2};
constexpr unsigned int c_maxResumeReconnectAttempts = 6;

// The cancellation handle owns this context. An admitted callback retains it
// before disassociating, so cancellation may drain safely even from the callback.
// The timer is closed only after the last admitted callback releases the context.
struct NativeSchedule : std::enable_shared_from_this<NativeSchedule> {
    PowerTransitionCoordinator::Tick Deliver;
    wil::unique_threadpool_timer Timer;

    static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
        auto lifetime = static_cast<NativeSchedule*>(context)->shared_from_this();
        DisassociateCurrentThreadFromCallback(instance);
        util::RuntimeApartment apartment;
        if (!apartment.Ready()) {
            DebugTrace(L"[PowerTransitionCoordinator] Resume callback apartment unavailable; retrying next tick");
            return;
        }
        if (!lifetime->Deliver()) SetThreadpoolTimer(lifetime->Timer.get(), nullptr, 0, 0);
    }
};

PowerTransitionCoordinator::CancelTimer ScheduleNative(std::chrono::milliseconds period,
                                                       PowerTransitionCoordinator::Tick tick) {
    auto context = std::make_shared<NativeSchedule>();
    context->Deliver = std::move(tick);
    context->Timer.reset(CreateThreadpoolTimer(NativeSchedule::OnTimer, context.get(), nullptr));
    if (!context->Timer) return {};
    LARGE_INTEGER relative{};
    relative.QuadPart = -period.count() * 10'000LL;
    FILETIME dueTime{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
    PowerTransitionCoordinator::CancelTimer cancel = [context]() noexcept {
        SetThreadpoolTimer(context->Timer.get(), nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(context->Timer.get(), TRUE);
    };
    SetThreadpoolTimer(context->Timer.get(), &dueTime, static_cast<DWORD>(period.count()), 0);
    return cancel;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Recovery State ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct PowerTransitionCoordinator::ResumeState {
    // No nested locks. Callbacks, capture destruction and logging occur unlocked.
    std::mutex Mutex;
    ResumeReconnectAttemptState Attempts;
    std::uint64_t Generation = 0;
    std::uint64_t DeliverySequence = 0;
    std::shared_ptr<const ResumeReconnectCallback> Reconnect;
    bool DeliveryInFlight = false;
    bool Cancelled = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

PowerTransitionCoordinator::PowerTransitionCoordinator(std::atomic<bool>& exiting, Scheduler scheduler)
    : m_exiting(exiting), m_resumeState(std::make_shared<ResumeState>()),
      m_schedule(scheduler ? std::move(scheduler) : Scheduler{ScheduleNative}) {}

PowerTransitionCoordinator::~PowerTransitionCoordinator() {
    Cancel();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::Cancel() noexcept {
    std::shared_ptr<const ResumeReconnectCallback> retired;
    {
        std::scoped_lock lock(m_resumeState->Mutex);
        m_resumeState->Cancelled = true;
        m_resumeState->Attempts.Clear();
        retired = std::move(m_resumeState->Reconnect);
        m_resumeState->DeliveryInFlight = false;
        ++m_resumeState->Generation;
    }
    CancelResumeReconnectTimer();
}

void PowerTransitionCoordinator::HandleSuspend(std::function<void()> flushSettings,
                                               std::function<std::vector<std::wstring>()> suspendDevices) noexcept {
    try {
        if (m_exiting.load() || m_powerSuspended) return;
        m_powerSuspended = true;
        std::shared_ptr<const ResumeReconnectCallback> retired;
        std::uint64_t generation;
        {
            std::scoped_lock lock(m_resumeState->Mutex);
            m_resumeState->Cancelled = true;
            retired = std::move(m_resumeState->Reconnect);
            m_resumeState->DeliveryInFlight = false;
            generation = ++m_resumeState->Generation;
        }
        CancelResumeReconnectTimer();
        if (flushSettings) flushSettings();
        auto activeDeviceIds = suspendDevices ? suspendDevices() : std::vector<std::wstring>{};
        {
            std::scoped_lock lock(m_resumeState->Mutex);
            if (m_resumeState->Generation != generation || m_exiting.load()) return;
            m_resumeState->Attempts.BeginCycle(std::move(activeDeviceIds));
            m_resumeState->Cancelled = false;
        }
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] Suspend failed; recovery generation remains invalidated");
    }
}

void PowerTransitionCoordinator::HandleResume(std::function<void()> resumeDevices,
                                              ResumeReconnectCallback reconnectAfterDelay) noexcept {
    try {
        if (m_exiting.load()) return;
        auto const now = std::chrono::steady_clock::now();
        if (!m_powerSuspended && m_lastResumeHandledAt != std::chrono::steady_clock::time_point{} &&
            now - m_lastResumeHandledAt < c_duplicateResumeWindow)
            return;
        m_lastResumeHandledAt = now;
        bool const matchedSuspend = std::exchange(m_powerSuspended, false);
        if (resumeDevices) resumeDevices();
        if (!matchedSuspend) return;
        CancelResumeReconnectTimer();

        auto state = m_resumeState;
        auto callback = std::make_shared<const ResumeReconnectCallback>(std::move(reconnectAfterDelay));
        std::uint64_t generation;
        {
            std::scoped_lock lock(state->Mutex);
            if (state->Cancelled || state->Attempts.Empty()) return;
            generation = state->Generation;
            state->Reconnect.swap(callback);
            state->DeliveryInFlight = false;
        }
        try {
            m_cancelTimer = m_schedule(c_resumeReconnectDelay, [state, generation]() noexcept {
                return DeliverResumeReconnect(state, generation);
            });
        } catch (...) {
            // A platform scheduler may report resource exhaustion by throwing.
        }
        if (!m_cancelTimer) {
            DebugTrace(L"[PowerTransitionCoordinator] Resume timer unavailable; delivering once immediately");
            static_cast<void>(DeliverResumeReconnect(state, generation));
        }
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] Resume scheduling failed");
    }
}

void PowerTransitionCoordinator::NotifyDeviceConnected(std::wstring_view deviceId) noexcept {
    if (deviceId.empty()) return;
    bool completed;
    {
        std::scoped_lock lock(m_resumeState->Mutex);
        static_cast<void>(m_resumeState->Attempts.Acknowledge(deviceId));
        completed = m_resumeState->Attempts.Empty();
    }
    if (completed) CancelResumeReconnectTimer();
}

bool PowerTransitionCoordinator::IsResumeReconnectGenerationCurrent(std::uint64_t generation) const noexcept {
    std::scoped_lock lock(m_resumeState->Mutex);
    return !m_resumeState->Cancelled && m_resumeState->Generation == generation && !m_resumeState->Attempts.Empty();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::CancelResumeReconnectTimer() noexcept {
    auto cancel = std::move(m_cancelTimer);
    if (cancel) cancel();
}

bool PowerTransitionCoordinator::DeliverResumeReconnect(std::shared_ptr<ResumeState> const& state,
                                                        std::uint64_t generation) noexcept {
    ResumeReconnectAttemptState::Selection selection;
    std::shared_ptr<const ResumeReconnectCallback> reconnect;
    std::uint64_t delivery;
    {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation || state->Attempts.Empty()) return false;
        if (state->DeliveryInFlight) return true;
        selection = state->Attempts.SelectEligible(c_maxResumeReconnectAttempts);
        reconnect = state->Reconnect;
        delivery = ++state->DeliverySequence;
        state->DeliveryInFlight = !selection.Eligible.empty() && reconnect && *reconnect;
    }
    for (auto const& id : selection.Exhausted) {
        DebugTrace(L"[PowerTransitionCoordinator] Resume reconnect retry limit reached for {0}", id);
    }
    if (selection.Eligible.empty()) return false;
    if (!reconnect || !*reconnect) return true;

    auto completed = [state, generation, delivery](std::vector<std::wstring> attemptedIds) noexcept {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation || state->DeliverySequence != delivery ||
            !state->DeliveryInFlight)
            return;
        state->Attempts.RecordAttempts(attemptedIds);
        state->DeliveryInFlight = false;
    };
    try {
        (*reconnect)(std::move(selection.Eligible), generation, completed);
    } catch (...) {
        completed({});
        DebugTrace(L"[PowerTransitionCoordinator] Delayed resume reconnect callback failed");
    }
    return true;
}
