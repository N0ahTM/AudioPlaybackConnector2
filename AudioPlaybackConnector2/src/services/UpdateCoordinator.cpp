#include <services/UpdateCoordinator.hpp>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {
template <typename Action> class ScopeExit {
public:
    explicit ScopeExit(Action action) noexcept(std::is_nothrow_move_constructible_v<Action>)
        : m_action(std::move(action)) {}
    ~ScopeExit() noexcept { m_action(); }

    ScopeExit(ScopeExit const&) = delete;
    ScopeExit& operator=(ScopeExit const&) = delete;

private:
    Action m_action;
};
} // namespace

UpdateCoordinator::Event::Event(bool manualReset)
    : m_handle(CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr)) {
    if (!m_handle) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
}

UpdateCoordinator::Event::~Event() {
    if (m_handle) CloseHandle(m_handle);
}

void UpdateCoordinator::Event::Signal() const noexcept {
    if (m_handle) SetEvent(m_handle);
}

UpdateCoordinator::Flight::Flight() = default;

UpdateCoordinator::UpdateCoordinator(CheckOperation checkOperation,
                                     std::chrono::steady_clock::duration automaticResultReuse)
    : m_checkOperation(std::move(checkOperation)), m_automaticResultReuse(automaticResultReuse) {
    if (!m_checkOperation) {
        throw std::invalid_argument("UpdateCoordinator requires a check operation");
    }
}

UpdateCoordinator::~UpdateCoordinator() {
    Shutdown();
}

UpdateCheckTask UpdateCoordinator::CheckForUpdatesAsync(UpdateCheckReason reason) {
    auto lifetime = shared_from_this();
    while (true) {
        std::shared_ptr<Flight> flight;
        std::optional<UpdateCheckResult> immediateResult;
        bool startFlight = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || (reason == UpdateCheckReason::Automatic && !m_automaticChecksAllowed)) {
                immediateResult = CancelledResult();
            } else if (reason == UpdateCheckReason::Automatic && m_lastSuccessfulResult &&
                       std::chrono::steady_clock::now() - m_lastSuccessfulResultAt <= m_automaticResultReuse) {
                immediateResult = *m_lastSuccessfulResult;
            } else {
                flight = m_currentFlight;
                if (!flight) {
                    flight = std::make_shared<Flight>();
                    m_currentFlight = flight;
                    startFlight = true;
                }
                if (reason == UpdateCheckReason::Manual) {
                    ++flight->ManualWaiters;
                }
            }
        }

        if (immediateResult) co_return std::move(*immediateResult);

        ScopeExit manualWaiterGuard([weak = weak_from_this(), flight, reason]() noexcept {
            if (reason != UpdateCheckReason::Manual) return;
            if (auto self = weak.lock()) self->ReleaseManualWaiter(flight);
        });

        if (startFlight) {
            try {
                RunFlightAsync(flight);
            } catch (...) {
                OutputDebugStringW(L"[UpdateCoordinator] failed to allocate update flight coroutine\n");
                CompleteFlight(flight, UpdateCheckResult{});
            }
        }
        co_await winrt::resume_on_signal(flight->Completed.Get());

        UpdateCheckResult result;
        bool stopping = false;
        {
            std::lock_guard lock(m_mutex);
            result = flight->Result;
            stopping = m_stopping;
        }

        if (reason == UpdateCheckReason::Manual && result.Status == UpdateCheckStatus::Cancelled && !stopping) {
            continue;
        }
        co_return result;
    }
}

winrt::Windows::Foundation::IAsyncOperation<bool>
UpdateCoordinator::WaitForAutomaticCheckWindowAsync(std::chrono::steady_clock::duration stableDelay) {
    auto lifetime = shared_from_this();
    AutomaticUpdateWindow window;

    while (true) {
        std::shared_ptr<Event> stateChanged;
        bool stopping = false;
        bool allowed = false;
        auto remaining = winrt::Windows::Foundation::TimeSpan::zero();
        {
            std::lock_guard lock(m_mutex);
            stopping = m_stopping;
            allowed = m_automaticChecksAllowed;
            const auto now = std::chrono::steady_clock::now();
            if (window.Update(allowed, stopping, now, stableDelay)) co_return true;
            if (stopping) co_return false;

            stateChanged = std::make_shared<Event>(true);
            std::erase_if(m_stateWaiters, [](auto const& waiter) { return waiter.expired(); });
            m_stateWaiters.emplace_back(stateChanged);
            if (allowed) {
                remaining = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                    window.Remaining(now, stableDelay));
            }
        }
        if (!allowed) {
            co_await winrt::resume_on_signal(stateChanged->Get());
            continue;
        }

        if (remaining <= winrt::Windows::Foundation::TimeSpan::zero()) co_return true;
        const auto signaled = co_await winrt::resume_on_signal(stateChanged->Get(), remaining);
        if (!signaled) {
            std::lock_guard lock(m_mutex);
            const auto now = std::chrono::steady_clock::now();
            if (window.Update(m_automaticChecksAllowed, m_stopping, now, stableDelay)) co_return true;
            if (m_stopping) co_return false;
            continue;
        }
        window.Reset();
    }
}

void UpdateCoordinator::SetAutomaticChecksAllowed(bool allowed) noexcept {
    std::shared_ptr<Flight> flightToCancel;
    try {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || m_automaticChecksAllowed == allowed) return;
            m_automaticChecksAllowed = allowed;
            SignalStateWaitersLocked();
            if (!allowed && m_currentFlight && m_currentFlight->ManualWaiters == 0) {
                flightToCancel = m_currentFlight;
            }
        }
        if (flightToCancel) flightToCancel->StopSource.request_stop();
    } catch (...) {
        OutputDebugStringW(L"[UpdateCoordinator] failed to change automatic-check policy\n");
    }
}

void UpdateCoordinator::Shutdown() noexcept {
    std::shared_ptr<Flight> flightToCancel;
    try {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
            flightToCancel = m_currentFlight;
            SignalStateWaitersLocked();
        }
        if (flightToCancel) flightToCancel->StopSource.request_stop();
    } catch (...) {
        OutputDebugStringW(L"[UpdateCoordinator] shutdown ignored exception\n");
    }
}

winrt::fire_and_forget UpdateCoordinator::RunFlightAsync(std::shared_ptr<Flight> flight) {
    std::shared_ptr<UpdateCoordinator> lifetime;
    UpdateCheckResult result;
    try {
        lifetime = shared_from_this();
        result = co_await m_checkOperation(flight->StopSource.get_token());
    } catch (winrt::hresult_error const& ex) {
        try {
            result.ErrorMessage = std::wstring(ex.message());
        } catch (...) {
        }
    } catch (std::exception const& ex) {
        try {
            auto const length = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
            if (length > 1) {
                result.ErrorMessage.resize(static_cast<size_t>(length));
                MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, result.ErrorMessage.data(), length);
                result.ErrorMessage.pop_back();
            }
        } catch (...) {
        }
    } catch (...) {
        try {
            result.ErrorMessage = L"Unknown update check error";
        } catch (...) {
        }
    }

    if (flight->StopSource.stop_requested()) {
        result = {};
        result.Status = UpdateCheckStatus::Cancelled;
    }

    CompleteFlight(flight, std::move(result));
}

void UpdateCoordinator::CompleteFlight(std::shared_ptr<Flight> const& flight, UpdateCheckResult&& result) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<UpdateCheckResult>);
    struct CompletionSignal {
        Event& Completed;
        ~CompletionSignal() { Completed.Signal(); }
    } completionSignal{flight->Completed};

    try {
        std::lock_guard lock(m_mutex);
        flight->Result = std::move(result);
        if (m_currentFlight == flight) m_currentFlight.reset();

        if (flight->Result.Status == UpdateCheckStatus::UpToDate ||
            flight->Result.Status == UpdateCheckStatus::UpdateAvailable) {
            try {
                m_lastSuccessfulResult = flight->Result;
                m_lastSuccessfulResultAt = std::chrono::steady_clock::now();
            } catch (...) {
                m_lastSuccessfulResult.reset();
                OutputDebugStringW(L"[UpdateCoordinator] result cache update failed\n");
            }
        }
    } catch (...) {
        OutputDebugStringW(L"[UpdateCoordinator] flight completion state update failed\n");
    }
}

void UpdateCoordinator::ReleaseManualWaiter(std::shared_ptr<Flight> const& flight) noexcept {
    std::shared_ptr<Flight> flightToCancel;
    try {
        {
            std::lock_guard lock(m_mutex);
            if (flight->ManualWaiters > 0) --flight->ManualWaiters;
            if (flight->ManualWaiters == 0 && !m_automaticChecksAllowed && m_currentFlight == flight) {
                flightToCancel = flight;
            }
        }
        if (flightToCancel) flightToCancel->StopSource.request_stop();
    } catch (...) {
    }
}

void UpdateCoordinator::SignalStateWaitersLocked() noexcept {
    for (auto const& weakWaiter : m_stateWaiters) {
        if (auto waiter = weakWaiter.lock()) waiter->Signal();
    }
    m_stateWaiters.clear();
}

UpdateCheckResult UpdateCoordinator::CancelledResult() {
    UpdateCheckResult result;
    result.Status = UpdateCheckStatus::Cancelled;
    result.ErrorMessage = L"Update check cancelled";
    return result;
}
