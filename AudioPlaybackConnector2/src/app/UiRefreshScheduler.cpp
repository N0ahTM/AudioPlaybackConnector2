#include <app/UiRefreshScheduler.hpp>
#include <util/RuntimeApartment.hpp>
#include <windows.h>
#include <wil/resource.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <system_error>
#include <utility>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Scheduler State ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct UiRefreshScheduler::State : std::enable_shared_from_this<State> {
    Dispatch Enqueue;
    Render Apply;
    Flags RetryMask;
    util::LogSink Log;
    UiRefreshCoalescer Pending;
    std::atomic_bool Stopped{false};
    std::atomic_uint64_t NextTicket{0};
    std::atomic_uint64_t QueuedTicket{0};
    // Only the UI drain accesses the render-failure count.
    unsigned int Failures = 0;
    // This lock only serializes native arm/disarm, never dispatcher or rendering calls.
    std::mutex TimerMutex;
    wil::unique_threadpool_timer Timer;

    State(Dispatch dispatch, Render render, Flags retryMask, util::LogSink log)
        : Enqueue(std::move(dispatch)), Apply(std::move(render)), RetryMask(retryMask), Log(std::move(log)) {}

    ~State() { Stop(); }

    static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
        auto self = static_cast<State*>(context)->shared_from_this();
        // A callback retains state before disassociating. Stop may therefore
        // drain admission without waiting on code that can enqueue UI work.
        DisassociateCurrentThreadFromCallback(instance);
        util::RuntimeApartment apartment;
        if (apartment.Ready())
            self->Queue();
        else
            self->Arm(std::chrono::milliseconds{100});
    }

    void Arm(std::chrono::milliseconds delay) noexcept {
        std::scoped_lock lock(TimerMutex);
        if (Stopped.load() || !Timer) return;
        LARGE_INTEGER relative{};
        relative.QuadPart = -delay.count() * 10'000LL;
        FILETIME due{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
        SetThreadpoolTimer(Timer.get(), &due, 0, 0);
    }

    void Stop() noexcept {
        Stopped.store(true);
        QueuedTicket.store(0);
        Pending.Cancel();
        wil::unique_threadpool_timer timer;
        {
            std::scoped_lock lock(TimerMutex);
            timer = std::move(Timer);
            if (timer) SetThreadpoolTimer(timer.get(), nullptr, 0, 0);
        }
        if (timer) WaitForThreadpoolTimerCallbacks(timer.get(), TRUE);
    }

    void Queue() noexcept {
        if (Stopped.load()) return;
        const auto ticket = NextTicket.fetch_add(1) + 1;
        QueuedTicket.store(ticket);
        try {
            if (Enqueue([weak = weak_from_this(), ticket] {
                    auto self = weak.lock();
                    if (!self) return;
                    auto expected = ticket;
                    if (!self->QueuedTicket.compare_exchange_strong(expected, 0) || self->Stopped.load()) return;
                    self->Drain();
                }))
                return;
        } catch (...) {
            Log.UnknownException(L"[UiRefreshScheduler] UI dispatch failed");
        }
        auto expected = ticket;
        QueuedTicket.compare_exchange_strong(expected, 0);
        // Keep the coalescing reservation: requests arriving during retry are
        // merged into the same eventual UI pass, with no lost wakeup.
        Arm(std::chrono::milliseconds{100});
    }

    void Drain() noexcept {
        const auto flags = Pending.BeginDrain();
        bool succeeded = false;
        try {
            succeeded = Apply(flags);
        } catch (...) {
            Log.UnknownException(L"[UiRefreshScheduler] UI rendering failed");
        }
        if (Stopped.load()) return;
        if (!succeeded) static_cast<void>(Pending.Request(flags & RetryMask));
        Failures = succeeded ? 0U : std::min(Failures + 1U, 6U);
        if (!Pending.CompleteDrain()) return;
        if (succeeded)
            Queue();
        else
            Arm(std::chrono::milliseconds{100U << (Failures - 1U)});
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Lifecycle and Requests ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

UiRefreshScheduler::UiRefreshScheduler(Dispatch dispatch, Render render, Flags retryMask, util::LogSink log)
    : m_state(std::make_shared<State>(std::move(dispatch), std::move(render), retryMask, std::move(log))) {
    m_state->Timer.reset(CreateThreadpoolTimer(State::OnTimer, m_state.get(), nullptr));
    if (!m_state->Timer) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
}

UiRefreshScheduler::~UiRefreshScheduler() {
    Stop();
}

void UiRefreshScheduler::Request(Flags flags) {
    auto state = m_state;
    if (state->Pending.Request(flags)) state->Queue();
}

void UiRefreshScheduler::Stop() noexcept {
    auto state = m_state;
    state->Stop();
}
