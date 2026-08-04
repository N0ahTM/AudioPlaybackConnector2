#include <pch.h>

#include <app/ApplicationHost.hpp>

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

bool ApplicationHost::ScheduleDeferredSettingsSaveTimer(DeferredSaveCoordinator::WorkerToken worker,
                                                        std::chrono::milliseconds delay,
                                                        bool resetFailures) noexcept {
    std::scoped_lock lock(m_settingsSaveTimerMutex);
    if (m_exiting.load()) return false;

    if (!m_settingsSaveTimer) {
        m_settingsSaveTimer.reset(CreateThreadpoolTimer(DeferredSettingsSaveTimerCallback, this, nullptr));
        if (!m_settingsSaveTimer) {
            m_settingsSaveWorker = worker;
            if (!m_hwnd || !IsWindow(m_hwnd)) return false;
            auto const fallbackDelay = static_cast<UINT>(std::clamp<std::int64_t>(delay.count(), 1, UINT_MAX));
            if (!SetTimer(m_hwnd, c_timerDeferredSettingsSaveRetry, fallbackDelay, nullptr)) return false;
            if (resetFailures) m_settingsSaveConsecutiveFailures = 0;
            DebugTrace(L"[App] Deferred settings save using Win32 timer fallback");
            return true;
        }
    }

    if (m_hwnd) KillTimer(m_hwnd, c_timerDeferredSettingsSaveRetry);
    m_settingsSaveWorker = worker;
    if (resetFailures) m_settingsSaveConsecutiveFailures = 0;
    auto dueTime = RelativeDueTime(delay);
    SetThreadpoolTimer(m_settingsSaveTimer.get(), &dueTime, 0, 0);
    return true;
}

void ApplicationHost::RunDeferredSettingsSaveAttempt(DeferredSaveCoordinator::WorkerToken worker) noexcept {
    if (m_exiting.load()) return;

    auto attempt = m_settingsSaveCoordinator.BeginAttempt(worker);
    if (!attempt) return;

    util::RuntimeApartment apartment;
    auto settings = m_settings;
    auto const saved = apartment.Ready() && settings && settings->Save(GetModuleHandleW(nullptr));
    if (!apartment.Ready()) {
        DebugTrace(L"[App] Deferred settings save could not initialize WinRT apartment: 0x{0:08X}",
                   static_cast<std::uint32_t>(apartment.Result()));
    }
    auto const completion = m_settingsSaveCoordinator.CompleteAttempt(*attempt, saved);

    switch (completion) {
        case DeferredSaveCoordinator::Completion::Stop: {
            std::scoped_lock lock(m_settingsSaveTimerMutex);
            if (m_settingsSaveWorker == worker) {
                m_settingsSaveWorker.reset();
                m_settingsSaveConsecutiveFailures = 0;
            }
            return;
        }
        case DeferredSaveCoordinator::Completion::Stale: return;
        case DeferredSaveCoordinator::Completion::ContinueAfterDebounce:
            if (!ScheduleDeferredSettingsSaveTimer(worker, std::chrono::milliseconds(300), true)) {
                static_cast<void>(m_settingsSaveCoordinator.AbandonWorker(worker));
                DebugTrace(L"[App] Deferred settings follow-up could not be scheduled; flushing synchronously");
                static_cast<void>(FlushSettingsNow(3));
            }
            return;
        case DeferredSaveCoordinator::Completion::RetryWithBackoff: {
            unsigned int failures = 0;
            bool workerStillCurrent = false;
            {
                std::scoped_lock lock(m_settingsSaveTimerMutex);
                workerStillCurrent = m_settingsSaveWorker == worker;
                if (workerStillCurrent) {
                    m_settingsSaveConsecutiveFailures = std::min(m_settingsSaveConsecutiveFailures + 1U, 10U);
                    failures = m_settingsSaveConsecutiveFailures;
                }
            }
            if (!workerStillCurrent) {
                static_cast<void>(m_settingsSaveCoordinator.AbandonWorker(worker));
                return;
            }
            auto const multiplier = 1LL << (failures - 1U);
            auto const delay = std::min(std::chrono::milliseconds(1000) * multiplier,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(5)));
            if (ScheduleDeferredSettingsSaveTimer(worker, delay)) {
                DebugTrace(L"[App] Deferred settings save failed; retrying in {0} ms", delay.count());
            } else {
                static_cast<void>(m_settingsSaveCoordinator.AbandonWorker(worker));
                DebugTrace(L"[App] Deferred settings retry could not be scheduled; flushing synchronously");
                static_cast<void>(FlushSettingsNow(3));
            }
            return;
        }
    }
}

void ApplicationHost::CancelDeferredSettingsSaveTimer() noexcept {
    if (m_hwnd) KillTimer(m_hwnd, c_timerDeferredSettingsSaveRetry);
    wil::unique_threadpool_timer timer;
    {
        std::scoped_lock lock(m_settingsSaveTimerMutex);
        m_settingsSaveWorker.reset();
        m_settingsSaveConsecutiveFailures = 0;
        timer = std::move(m_settingsSaveTimer);
    }
    timer.reset();
}

void CALLBACK ApplicationHost::DeferredSettingsSaveTimerCallback(PTP_CALLBACK_INSTANCE,
                                                                 void* context,
                                                                 PTP_TIMER) noexcept {
    auto self = static_cast<ApplicationHost*>(context);
    if (!self || self->m_exiting.load()) return;

    std::optional<DeferredSaveCoordinator::WorkerToken> worker;
    {
        std::scoped_lock lock(self->m_settingsSaveTimerMutex);
        worker = self->m_settingsSaveWorker;
    }
    if (worker) self->RunDeferredSettingsSaveAttempt(*worker);
}

bool ApplicationHost::ScheduleNativeDeviceVisualRefreshRetry(std::chrono::milliseconds delay) noexcept {
    std::scoped_lock lock(m_deviceVisualRefreshRetryTimerMutex);
    if (m_exiting.load()) return false;
    if (!m_deviceVisualRefreshRetryTimer) {
        m_deviceVisualRefreshRetryTimer.reset(
            CreateThreadpoolTimer(DeviceVisualRefreshRetryTimerCallback, this, nullptr));
        if (!m_deviceVisualRefreshRetryTimer) return false;
    }
    auto dueTime = RelativeDueTime(delay);
    SetThreadpoolTimer(m_deviceVisualRefreshRetryTimer.get(), &dueTime, 0, 0);
    return true;
}

void ApplicationHost::CancelNativeDeviceVisualRefreshRetry() noexcept {
    wil::unique_threadpool_timer timer;
    {
        std::scoped_lock lock(m_deviceVisualRefreshRetryTimerMutex);
        timer = std::move(m_deviceVisualRefreshRetryTimer);
    }
    timer.reset();
}

void CALLBACK ApplicationHost::DeviceVisualRefreshRetryTimerCallback(PTP_CALLBACK_INSTANCE,
                                                                     void* context,
                                                                     PTP_TIMER) noexcept {
    auto self = static_cast<ApplicationHost*>(context);
    if (self && !self->m_exiting.load()) self->QueueDeviceVisualRefreshDrain();
}
