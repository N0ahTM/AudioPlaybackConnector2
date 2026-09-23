#include <app/PowerTransitionCoordinator.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
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
    util::LogSink Log;
    PowerTransitionCoordinator::Tick Deliver;
    wil::unique_threadpool_timer Timer;

    static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
        auto lifetime = static_cast<NativeSchedule*>(context)->shared_from_this();
        DisassociateCurrentThreadFromCallback(instance);
        util::RuntimeApartment apartment;
        if (!apartment.Ready()) {
            lifetime->Log.Trace(
                L"[PowerTransitionCoordinator] Resume callback apartment unavailable; retrying next tick");
            return;
        }
        if (!lifetime->Deliver()) SetThreadpoolTimer(lifetime->Timer.get(), nullptr, 0, 0);
    }
};

PowerTransitionCoordinator::CancelTimer
ScheduleNative(std::chrono::milliseconds period, PowerTransitionCoordinator::Tick tick, util::LogSink log) {
    auto context = std::make_shared<NativeSchedule>();
    context->Log = std::move(log);
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
    util::LogSink Log;
    // No nested locks. Callbacks, capture destruction and logging occur unlocked.
    std::mutex Mutex;
    struct Target {
        std::wstring Id;
        unsigned int Attempts = 0;
    };
    struct Selection {
        std::vector<std::wstring> Eligible;
        std::vector<std::wstring> Exhausted;
    };
    std::vector<Target> Targets;

    void BeginCycle(std::vector<std::wstring> activeDeviceIds) {
        for (auto& target : Targets)
            target.Attempts = 0;
        for (auto& id : activeDeviceIds) {
            if (!id.empty() && std::ranges::find(Targets, id, &Target::Id) == Targets.end())
                Targets.push_back({std::move(id)});
        }
    }

    Selection SelectEligible() {
        Selection result;
        for (auto const& target : Targets) {
            (target.Attempts >= c_maxResumeReconnectAttempts ? result.Exhausted : result.Eligible).push_back(target.Id);
        }
        std::erase_if(Targets, [](auto const& target) { return target.Attempts >= c_maxResumeReconnectAttempts; });
        return result;
    }

    void RecordAttempts(std::vector<std::wstring> const& attemptedIds) {
        for (auto& target : Targets) {
            if (std::ranges::find(attemptedIds, target.Id) != attemptedIds.end()) ++target.Attempts;
        }
    }
    std::uint64_t Generation = 0;
    std::uint64_t DeliverySequence = 0;
    std::shared_ptr<const ResumeReconnectCallback> Reconnect;
    bool DeliveryInFlight = false;
    bool Cancelled = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

PowerTransitionCoordinator::PowerTransitionCoordinator(std::atomic<bool>& exiting,
                                                       Scheduler scheduler,
                                                       util::LogSink log)
    : m_exiting(exiting), m_resumeState(std::make_shared<ResumeState>()),
      m_schedule(scheduler ? std::move(scheduler) : Scheduler{[log](std::chrono::milliseconds period, Tick tick) {
          return ScheduleNative(period, std::move(tick), log);
      }}) {
    m_resumeState->Log = std::move(log);
}

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
        m_resumeState->Targets.clear();
        retired = std::move(m_resumeState->Reconnect);
        m_resumeState->DeliveryInFlight = false;
        ++m_resumeState->Generation;
    }
    CancelResumeReconnectTimer();
}

void PowerTransitionCoordinator::HandleSuspend(
    std::function<void()> const& flushSettings,
    std::function<std::vector<std::wstring>()> const& suspendDevices) noexcept {
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
            m_resumeState->BeginCycle(std::move(activeDeviceIds));
            m_resumeState->Cancelled = false;
        }
    } catch (...) {
        m_resumeState->Log.Trace(
            L"[PowerTransitionCoordinator] Suspend failed; recovery generation remains invalidated");
    }
}

void PowerTransitionCoordinator::HandleResume(std::function<void()> const& resumeDevices,
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
            if (state->Cancelled || state->Targets.empty()) return;
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
            m_resumeState->Log.Trace(
                L"[PowerTransitionCoordinator] Resume timer unavailable; delivering once immediately");
            static_cast<void>(DeliverResumeReconnect(state, generation));
        }
    } catch (...) {
        m_resumeState->Log.Trace(L"[PowerTransitionCoordinator] Resume scheduling failed");
    }
}

void PowerTransitionCoordinator::NotifyDeviceConnected(std::wstring_view deviceId) noexcept {
    if (deviceId.empty()) return;
    bool completed;
    {
        std::scoped_lock lock(m_resumeState->Mutex);
        std::erase_if(m_resumeState->Targets, [&](auto const& target) { return target.Id == deviceId; });
        completed = m_resumeState->Targets.empty();
    }
    if (completed) CancelResumeReconnectTimer();
}

bool PowerTransitionCoordinator::IsResumeReconnectGenerationCurrent(std::uint64_t generation) const noexcept {
    std::scoped_lock lock(m_resumeState->Mutex);
    return !m_resumeState->Cancelled && m_resumeState->Generation == generation && !m_resumeState->Targets.empty();
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
    ResumeState::Selection selection;
    std::shared_ptr<const ResumeReconnectCallback> reconnect;
    std::uint64_t delivery;
    {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation || state->Targets.empty()) return false;
        if (state->DeliveryInFlight) return true;
        selection = state->SelectEligible();
        reconnect = state->Reconnect;
        delivery = ++state->DeliverySequence;
        state->DeliveryInFlight = !selection.Eligible.empty() && reconnect && *reconnect;
    }
    for (auto const& id : selection.Exhausted) {
        state->Log.Trace(L"[PowerTransitionCoordinator] Resume reconnect retry limit reached for {0}", id);
    }
    if (selection.Eligible.empty()) return false;
    if (!reconnect || !*reconnect) return true;

    auto completed = [state, generation, delivery](std::vector<std::wstring> const& attemptedIds) noexcept {
        std::scoped_lock lock(state->Mutex);
        if (state->Cancelled || state->Generation != generation || state->DeliverySequence != delivery ||
            !state->DeliveryInFlight)
            return;
        state->RecordAttempts(attemptedIds);
        state->DeliveryInFlight = false;
    };
    try {
        (*reconnect)(std::move(selection.Eligible), generation, completed);
    } catch (...) {
        completed({});
        state->Log.Trace(L"[PowerTransitionCoordinator] Delayed resume reconnect callback failed");
    }
    return true;
}
