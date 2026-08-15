#include <pch.h>

#include <app/ApplicationHost.hpp>

namespace {
FILETIME RelativeDueTime(std::chrono::milliseconds delay) noexcept {
    auto const clampedDelay = std::max(delay, std::chrono::milliseconds(1));
    LARGE_INTEGER relative{};
    relative.QuadPart = -static_cast<LONGLONG>(clampedDelay.count()) * 10'000LL;
    return FILETIME{relative.LowPart, static_cast<DWORD>(relative.HighPart)};
}
} // namespace

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
