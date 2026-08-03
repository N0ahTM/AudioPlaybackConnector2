#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

enum class MemoryPressureState { Unknown, Low, Neutral, High };

enum class UserActivityState {
    Unknown,
    Available,
    NotPresent,
    Busy,
    Fullscreen,
    Presentation,
    QuietTime,
    ImmersiveApp
};

struct ResourcePressureValues {
    MemoryPressureState Memory = MemoryPressureState::Unknown;
    UserActivityState UserActivity = UserActivityState::Unknown;
    std::optional<bool> EnergySaver;

    [[nodiscard]] bool IsMemoryPressure() const noexcept;
    [[nodiscard]] bool HasReliableMemoryState() const noexcept;
    [[nodiscard]] bool IsBackgroundConstrained() const noexcept;
    [[nodiscard]] bool CanPreload() const noexcept;
    bool operator==(ResourcePressureValues const&) const = default;
};

struct ResourcePressureProbe {
    std::optional<bool> LowMemorySignaled;
    std::optional<bool> HighMemorySignaled;
    std::optional<UserActivityState> UserActivity;
    std::optional<bool> EnergySaver;
    bool MemoryProbeAttempted = false;
    bool UserActivityProbeAttempted = false;
    bool EnergySaverProbeAttempted = false;
};

struct MemoryNotificationWaitPlan {
    bool ArmLow = false;
    bool ArmHigh = false;
};

class ResourcePressureStateReducer {
public:
    [[nodiscard]] ResourcePressureValues Apply(ResourcePressureProbe const& probe) noexcept;
    [[nodiscard]] ResourcePressureValues const& Current() const noexcept;

private:
    ResourcePressureValues m_current;
};

[[nodiscard]] bool IsResourcePressureSnapshotFresh(std::optional<std::chrono::steady_clock::time_point> observedAt,
                                                   std::chrono::steady_clock::time_point now,
                                                   std::chrono::milliseconds maximumAge) noexcept;
[[nodiscard]] bool IsPositiveResourceAuthorizationCurrent(std::uint64_t appliedSequence,
                                                          std::uint64_t latestConstrainedSequence) noexcept;
[[nodiscard]] bool ShouldUseConstrainedPollInterval(ResourcePressureValues const& values, bool probesComplete) noexcept;
[[nodiscard]] MemoryNotificationWaitPlan PlanMemoryNotificationWaits(std::optional<bool> lowMemorySignaled,
                                                                     std::optional<bool> highMemorySignaled) noexcept;
