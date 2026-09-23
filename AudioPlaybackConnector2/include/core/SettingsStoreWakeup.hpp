#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Persistence Clock and Wakeup //////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// One waiter serves the persistence worker. The store calls this boundary without
// holding its state or publication locks. Deadlines belong to the Now() clock;
// shutdown budgets use the real steady clock independently.
class SettingsStoreWakeup {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~SettingsStoreWakeup() = default;
    [[nodiscard]] virtual Clock::time_point Now() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t Version() const noexcept = 0;
    virtual void Notify() noexcept = 0;
    // Return when the version changes or the deadline expires. A notification
    // between Version() and Wait() must be retained, including before wait entry.
    virtual void Wait(std::uint64_t version, std::optional<Clock::time_point> deadline) noexcept = 0;
};
