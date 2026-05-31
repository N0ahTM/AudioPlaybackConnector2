#include <pch.h>

#include <app/PowerTransitionCoordinator.hpp>

#include <core/DeviceManager.hpp>

namespace {
constexpr std::chrono::seconds c_resumeReconnectDelay{10};
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

PowerTransitionCoordinator::PowerTransitionCoordinator(std::atomic<bool>& exiting) noexcept : m_exiting(exiting) {}

PowerTransitionCoordinator::~PowerTransitionCoordinator() {
    Cancel();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::Cancel() noexcept {
    CancelResumeReconnectTimer();
}

void PowerTransitionCoordinator::HandleSuspend(std::function<void()> saveLastConnectedDevices,
                                               std::shared_ptr<DeviceManager> deviceManager) noexcept {
    try {
        if (m_exiting.load() || m_powerSuspended) return;
        m_powerSuspended = true;
        DebugTrace(L"[PowerTransitionCoordinator] Power suspend detected");

        CancelResumeReconnectTimer();

        if (saveLastConnectedDevices) {
            saveLastConnectedDevices();
        }
        if (deviceManager) {
            deviceManager->SuspendForPowerTransition();
        }
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] HandleSuspend ERROR: ignored exception");
    }
}

void PowerTransitionCoordinator::HandleResume(std::shared_ptr<DeviceManager> deviceManager,
                                              std::function<void()> reconnectAfterDelay) noexcept {
    try {
        if (m_exiting.load()) return;

        if (m_powerSuspended) {
            m_powerSuspended = false;
            DebugTrace(L"[PowerTransitionCoordinator] Power resume detected");
        } else {
            // Some systems may surface resume without a matching suspend callback. Treat this as a recovery point.
            DebugTrace(L"[PowerTransitionCoordinator] Power resume detected without prior suspend; running recovery");
        }

        if (deviceManager) {
            deviceManager->ResumeAfterPowerTransition();
        }

        CancelResumeReconnectTimer();

        m_resumeReconnectTimer = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [reconnectAfterDelay = std::move(reconnectAfterDelay)](auto) mutable {
                if (reconnectAfterDelay) {
                    reconnectAfterDelay();
                }
            },
            c_resumeReconnectDelay);
    } catch (...) {
        DebugTrace(L"[PowerTransitionCoordinator] HandleResume ERROR: ignored exception");
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void PowerTransitionCoordinator::CancelResumeReconnectTimer() noexcept {
    auto timer = std::exchange(m_resumeReconnectTimer, nullptr);
    if (!timer) return;

    try {
        timer.Cancel();
    } catch (...) {
    }
}
