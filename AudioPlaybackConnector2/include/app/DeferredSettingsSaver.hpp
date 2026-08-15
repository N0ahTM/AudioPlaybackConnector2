#pragma once

#include <app/DeferredSaveCoordinator.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include <wil/resource.h>

class Settings;

class DeferredSettingsSaver {
public:
    explicit DeferredSettingsSaver(std::atomic<bool> const& stopping) noexcept;
    ~DeferredSettingsSaver();

    DeferredSettingsSaver(DeferredSettingsSaver const&) = delete;
    DeferredSettingsSaver& operator=(DeferredSettingsSaver const&) = delete;

    void Initialize(std::shared_ptr<Settings> settings, HWND hwnd) noexcept;
    void RequestSave() noexcept;
    [[nodiscard]] bool FlushNow(unsigned int maximumAttempts = 1) noexcept;
    void Cancel() noexcept;
    [[nodiscard]] bool HandleWindowTimer(UINT_PTR timerId) noexcept;

private:
    [[nodiscard]] bool ScheduleTimer(DeferredSaveCoordinator::WorkerToken worker,
                                     std::chrono::milliseconds delay,
                                     bool resetFailures = false) noexcept;
    void RunAttempt(DeferredSaveCoordinator::WorkerToken worker) noexcept;
    static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) noexcept;

    static constexpr UINT_PTR c_windowTimerId = 0x41504336;

    std::atomic<bool> const& m_stopping;
    std::shared_ptr<Settings> m_settings;
    HWND m_hwnd = nullptr;
    DeferredSaveCoordinator m_coordinator;
    std::mutex m_timerMutex;
    wil::unique_threadpool_timer m_timer;
    std::optional<DeferredSaveCoordinator::WorkerToken> m_worker;
    unsigned int m_consecutiveFailures = 0;
};
