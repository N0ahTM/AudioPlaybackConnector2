#pragma once

#include <app/ResumeReconnectAttemptState.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class DeviceManager;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Power Transition Coordinator //////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class PowerTransitionCoordinator {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors /////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ResumeReconnectCompleted = std::function<void(std::vector<std::wstring>)>;
    using ResumeReconnectCallback =
        std::function<void(std::vector<std::wstring>, std::uint64_t, ResumeReconnectCompleted)>;

    explicit PowerTransitionCoordinator(std::atomic<bool>& exiting);
    ~PowerTransitionCoordinator();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Cancel() noexcept;
    void HandleSuspend(std::function<void()> flushSettings, std::shared_ptr<DeviceManager> deviceManager) noexcept;
    void HandleResume(std::shared_ptr<DeviceManager> deviceManager,
                      ResumeReconnectCallback reconnectAfterDelay) noexcept;
    void NotifyDeviceConnected(std::wstring_view deviceId) noexcept;
    [[nodiscard]] bool IsResumeReconnectGenerationCurrent(std::uint64_t generation) const noexcept;

private:
    struct ResumeState;
    enum class DeliveryResult { Continue, Stop, CallbackDisassociated };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void CancelResumeReconnectTimer() noexcept;
    [[nodiscard]] static DeliveryResult
    DeliverResumeReconnect(std::shared_ptr<ResumeState> const& state,
                           std::uint64_t generation,
                           PTP_CALLBACK_INSTANCE callbackInstance = nullptr) noexcept;
    static void CALLBACK NativeResumeReconnectTimerCallback(PTP_CALLBACK_INSTANCE,
                                                            void* context,
                                                            PTP_TIMER timer) noexcept;

    struct ResumeState {
        std::mutex Mutex;
        ResumeReconnectAttemptState Attempts;
        std::uint64_t Generation = 0;
        ResumeReconnectCallback Reconnect;
        bool DeliveryInFlight = false;
        bool Cancelled = false;
    };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::atomic<bool>& m_exiting;
    bool m_powerSuspended = false;
    std::shared_ptr<ResumeState> m_resumeState;
    std::chrono::steady_clock::time_point m_lastResumeHandledAt{};
    winrt::Windows::System::Threading::ThreadPoolTimer m_resumeReconnectTimer{nullptr};
    wil::unique_threadpool_timer m_nativeResumeReconnectTimer;
};
