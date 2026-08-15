#pragma once

#include <app/ResourcePressureState.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

struct ResourcePressureSnapshot {
    ResourcePressureValues Values;
    std::chrono::steady_clock::time_point ObservedAt{};
    std::uint64_t Sequence = 0;
};

class ResourcePressureMonitor {
public:
    struct Config {
        std::chrono::milliseconds PollInterval{std::chrono::seconds{5}};
        std::chrono::milliseconds ConstrainedPollInterval{std::chrono::seconds{30}};
        std::chrono::milliseconds SnapshotHeartbeatInterval{std::chrono::seconds{30}};
    };

    using Callback = std::function<void(ResourcePressureSnapshot const&)>;

    explicit ResourcePressureMonitor(Callback callback, Config config = {});
    ~ResourcePressureMonitor();

    ResourcePressureMonitor(ResourcePressureMonitor const&) = delete;
    ResourcePressureMonitor& operator=(ResourcePressureMonitor const&) = delete;

    [[nodiscard]] bool Start() noexcept;
    [[nodiscard]] bool RequestProbe() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
