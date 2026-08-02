#pragma once

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
};

class ResourcePressureStateReducer {
public:
    [[nodiscard]] ResourcePressureValues Apply(ResourcePressureProbe const& probe) noexcept;
    [[nodiscard]] ResourcePressureValues const& Current() const noexcept;

private:
    ResourcePressureValues m_current;
};
