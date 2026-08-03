#pragma once

#include <services/UpdateService.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <windows.h>
#include <winrt/Windows.Foundation.h>

enum class UpdateCheckReason { Automatic, Manual };

class AutomaticUpdateWindow {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] bool Update(bool allowed, bool stopping, TimePoint now, Clock::duration stableDelay) noexcept;
    [[nodiscard]] Clock::duration Remaining(TimePoint now, Clock::duration stableDelay) const noexcept;
    void Reset() noexcept;

private:
    std::optional<TimePoint> m_allowedSince;
};

class UpdateCoordinator : public std::enable_shared_from_this<UpdateCoordinator> {
public:
    using CheckOperation = std::function<UpdateCheckTask(std::stop_token)>;

    explicit UpdateCoordinator(CheckOperation checkOperation,
                               std::chrono::steady_clock::duration automaticResultReuse = std::chrono::minutes(5));
    ~UpdateCoordinator();

    UpdateCoordinator(UpdateCoordinator const&) = delete;
    UpdateCoordinator& operator=(UpdateCoordinator const&) = delete;

    UpdateCheckTask CheckForUpdatesAsync(UpdateCheckReason reason);
    winrt::Windows::Foundation::IAsyncOperation<bool>
    WaitForAutomaticCheckWindowAsync(std::chrono::steady_clock::duration stableDelay);

    void SetAutomaticChecksAllowed(bool allowed) noexcept;
    void Shutdown() noexcept;

private:
    class Event {
    public:
        explicit Event(bool manualReset);
        ~Event();

        Event(Event const&) = delete;
        Event& operator=(Event const&) = delete;

        [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
        void Signal() const noexcept;

    private:
        HANDLE m_handle = nullptr;
    };

    struct Flight {
        Flight();

        Event Completed{true};
        std::stop_source StopSource;
        UpdateCheckResult Result;
        uint32_t ManualWaiters = 0;
    };

    winrt::fire_and_forget RunFlightAsync(std::shared_ptr<Flight> flight);
    void CompleteFlight(std::shared_ptr<Flight> const& flight, UpdateCheckResult&& result) noexcept;
    void ReleaseManualWaiter(std::shared_ptr<Flight> const& flight) noexcept;
    void SignalStateWaitersLocked() noexcept;
    [[nodiscard]] static UpdateCheckResult CancelledResult();

    CheckOperation m_checkOperation;
    std::chrono::steady_clock::duration m_automaticResultReuse;
    std::mutex m_mutex;
    std::shared_ptr<Flight> m_currentFlight;
    std::optional<UpdateCheckResult> m_lastSuccessfulResult;
    std::chrono::steady_clock::time_point m_lastSuccessfulResultAt{};
    std::vector<std::weak_ptr<Event>> m_stateWaiters;
    bool m_automaticChecksAllowed = true;
    bool m_stopping = false;
};
