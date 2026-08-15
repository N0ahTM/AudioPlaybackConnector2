#include <pch.h>

#include <app/DeferredSettingsSaver.hpp>

#include <core/Settings.hpp>
#include <util/RuntimeApartment.hpp>
#include <util/Util.hpp>

namespace {
FILETIME RelativeDueTime(std::chrono::milliseconds delay) noexcept {
    auto const clampedDelay = std::max(delay, std::chrono::milliseconds(1));
    LARGE_INTEGER relative{};
    relative.QuadPart = -static_cast<LONGLONG>(clampedDelay.count()) * 10'000LL;
    return FILETIME{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
}
} // namespace

DeferredSettingsSaver::DeferredSettingsSaver(std::atomic<bool> const& stopping) noexcept : m_stopping(stopping) {}

DeferredSettingsSaver::~DeferredSettingsSaver() {
    Cancel();
}

void DeferredSettingsSaver::Initialize(std::shared_ptr<Settings> settings, HWND hwnd) noexcept {
    std::scoped_lock lock(m_timerMutex);
    m_settings = std::move(settings);
    m_hwnd = hwnd;
}

void DeferredSettingsSaver::RequestSave() noexcept {
    if (m_stopping.load() || !m_settings) return;

    auto request = m_coordinator.MarkDirty();
    if (!request.WorkerToStart) return;

    if (!ScheduleTimer(*request.WorkerToStart, std::chrono::milliseconds(300), true)) {
        DebugTrace(L"[App] Deferred settings timer unavailable; saving synchronously");
        RunAttempt(*request.WorkerToStart);
    }
}

bool DeferredSettingsSaver::FlushNow(unsigned int maximumAttempts) noexcept {
    auto settings = m_settings;
    if (!settings) return true;

    auto const saveToken = m_coordinator.BeginExternalSave();
    maximumAttempts = std::max(maximumAttempts, 1U);
    for (unsigned int attempt = 1; attempt <= maximumAttempts; ++attempt) {
        try {
            if (settings->Save(GetModuleHandleW(nullptr))) {
                auto const clean = !settings->HasUnsavedChanges();
                static_cast<void>(m_coordinator.CompleteExternalSave(saveToken, clean));
                if (clean) return true;
            }
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[App] Synchronous settings flush ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[App] Synchronous settings flush ERROR", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[App] Synchronous settings flush ERROR");
        }
        DebugTrace(L"[App] Synchronous settings flush attempt {0}/{1} failed", attempt, maximumAttempts);
        if (attempt < maximumAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50U << (attempt - 1U)));
        }
    }
    return false;
}

void DeferredSettingsSaver::Cancel() noexcept {
    m_coordinator.Cancel();
    if (m_hwnd) KillTimer(m_hwnd, c_windowTimerId);

    wil::unique_threadpool_timer timer;
    {
        std::scoped_lock lock(m_timerMutex);
        m_worker.reset();
        m_consecutiveFailures = 0;
        timer = std::move(m_timer);
    }
    timer.reset();
}

bool DeferredSettingsSaver::HandleWindowTimer(UINT_PTR timerId) noexcept {
    if (timerId != c_windowTimerId) return false;

    if (m_hwnd) KillTimer(m_hwnd, c_windowTimerId);
    std::optional<DeferredSaveCoordinator::WorkerToken> worker;
    {
        std::scoped_lock lock(m_timerMutex);
        worker = m_worker;
    }
    if (worker) RunAttempt(*worker);
    return true;
}

bool DeferredSettingsSaver::ScheduleTimer(DeferredSaveCoordinator::WorkerToken worker,
                                          std::chrono::milliseconds delay,
                                          bool resetFailures) noexcept {
    std::scoped_lock lock(m_timerMutex);
    if (m_stopping.load()) return false;

    if (!m_timer) {
        m_timer.reset(CreateThreadpoolTimer(TimerCallback, this, nullptr));
        if (!m_timer) {
            m_worker = worker;
            if (!m_hwnd || !IsWindow(m_hwnd)) return false;
            auto const fallbackDelay = static_cast<UINT>(std::clamp<std::int64_t>(delay.count(), 1, UINT_MAX));
            if (!SetTimer(m_hwnd, c_windowTimerId, fallbackDelay, nullptr)) return false;
            if (resetFailures) m_consecutiveFailures = 0;
            DebugTrace(L"[App] Deferred settings save using Win32 timer fallback");
            return true;
        }
    }

    if (m_hwnd) KillTimer(m_hwnd, c_windowTimerId);
    m_worker = worker;
    if (resetFailures) m_consecutiveFailures = 0;
    auto dueTime = RelativeDueTime(delay);
    SetThreadpoolTimer(m_timer.get(), &dueTime, 0, 0);
    return true;
}

void DeferredSettingsSaver::RunAttempt(DeferredSaveCoordinator::WorkerToken worker) noexcept {
    if (m_stopping.load()) return;

    auto attempt = m_coordinator.BeginAttempt(worker);
    if (!attempt) return;

    util::RuntimeApartment apartment;
    auto settings = m_settings;
    auto const saved = apartment.Ready() && settings && settings->Save(GetModuleHandleW(nullptr));
    if (!apartment.Ready()) {
        DebugTrace(L"[App] Deferred settings save could not initialize WinRT apartment: 0x{0:08X}",
                   static_cast<std::uint32_t>(apartment.Result()));
    }
    auto const completion = m_coordinator.CompleteAttempt(*attempt, saved);

    switch (completion) {
        case DeferredSaveCoordinator::Completion::Stop: {
            std::scoped_lock lock(m_timerMutex);
            if (m_worker == worker) {
                m_worker.reset();
                m_consecutiveFailures = 0;
            }
            return;
        }
        case DeferredSaveCoordinator::Completion::Stale: return;
        case DeferredSaveCoordinator::Completion::ContinueAfterDebounce:
            if (!ScheduleTimer(worker, std::chrono::milliseconds(300), true)) {
                static_cast<void>(m_coordinator.AbandonWorker(worker));
                DebugTrace(L"[App] Deferred settings follow-up could not be scheduled; flushing synchronously");
                static_cast<void>(FlushNow(3));
            }
            return;
        case DeferredSaveCoordinator::Completion::RetryWithBackoff: {
            unsigned int failures = 0;
            bool workerStillCurrent = false;
            {
                std::scoped_lock lock(m_timerMutex);
                workerStillCurrent = m_worker == worker;
                if (workerStillCurrent) {
                    m_consecutiveFailures = std::min(m_consecutiveFailures + 1U, 10U);
                    failures = m_consecutiveFailures;
                }
            }
            if (!workerStillCurrent) {
                static_cast<void>(m_coordinator.AbandonWorker(worker));
                return;
            }
            auto const multiplier = 1LL << (failures - 1U);
            auto const delay = std::min(std::chrono::milliseconds(1000) * multiplier,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(5)));
            if (ScheduleTimer(worker, delay)) {
                DebugTrace(L"[App] Deferred settings save failed; retrying in {0} ms", delay.count());
            } else {
                static_cast<void>(m_coordinator.AbandonWorker(worker));
                DebugTrace(L"[App] Deferred settings retry could not be scheduled; flushing synchronously");
                static_cast<void>(FlushNow(3));
            }
            return;
        }
    }
}

void CALLBACK DeferredSettingsSaver::TimerCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept {
    auto self = static_cast<DeferredSettingsSaver*>(context);
    if (!self || self->m_stopping.load()) return;

    std::optional<DeferredSaveCoordinator::WorkerToken> worker;
    {
        std::scoped_lock lock(self->m_timerMutex);
        worker = self->m_worker;
    }
    if (worker) self->RunAttempt(*worker);
}
