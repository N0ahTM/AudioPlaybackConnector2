#include <app/ResourcePressureState.hpp>

bool ResourcePressureValues::IsMemoryPressure() const noexcept {
    return Memory == MemoryPressureState::Low;
}

bool ResourcePressureValues::HasReliableMemoryState() const noexcept {
    return Memory != MemoryPressureState::Unknown;
}

bool ResourcePressureValues::IsBackgroundConstrained() const noexcept {
    return IsMemoryPressure() || !HasReliableMemoryState() || EnergySaver.value_or(true) ||
           UserActivity == UserActivityState::Unknown || UserActivity == UserActivityState::NotPresent ||
           UserActivity == UserActivityState::Busy || UserActivity == UserActivityState::Fullscreen ||
           UserActivity == UserActivityState::Presentation || UserActivity == UserActivityState::ImmersiveApp;
}

bool ResourcePressureValues::CanPreload() const noexcept {
    return Memory == MemoryPressureState::High && EnergySaver == false && UserActivity == UserActivityState::Available;
}

ResourcePressureValues ResourcePressureStateReducer::Apply(ResourcePressureProbe const& probe) noexcept {
    if (probe.LowMemorySignaled == true) {
        m_current.Memory = MemoryPressureState::Low;
    } else if (probe.LowMemorySignaled && probe.HighMemorySignaled) {
        m_current.Memory = probe.HighMemorySignaled == true ? MemoryPressureState::High : MemoryPressureState::Neutral;
    } else if (probe.LowMemorySignaled == false ||
               (probe.HighMemorySignaled == false && m_current.Memory != MemoryPressureState::Low)) {
        m_current.Memory = MemoryPressureState::Neutral;
    } else if ((probe.MemoryProbeAttempted || probe.LowMemorySignaled || probe.HighMemorySignaled) &&
               m_current.Memory != MemoryPressureState::Low) {
        m_current.Memory = MemoryPressureState::Unknown;
    }

    if (probe.UserActivity) {
        m_current.UserActivity = *probe.UserActivity;
    } else if (probe.UserActivityProbeAttempted) {
        m_current.UserActivity = UserActivityState::Unknown;
    }
    if (probe.EnergySaver) {
        m_current.EnergySaver = *probe.EnergySaver;
    } else if (probe.EnergySaverProbeAttempted) {
        m_current.EnergySaver.reset();
    }
    return m_current;
}

ResourcePressureValues const& ResourcePressureStateReducer::Current() const noexcept {
    return m_current;
}

bool IsResourcePressureSnapshotFresh(std::optional<std::chrono::steady_clock::time_point> observedAt,
                                     std::chrono::steady_clock::time_point now,
                                     std::chrono::milliseconds maximumAge) noexcept {
    return observedAt && maximumAge > std::chrono::milliseconds::zero() && *observedAt <= now &&
           now - *observedAt < maximumAge;
}

bool IsPositiveResourceAuthorizationCurrent(std::uint64_t appliedSequence,
                                            std::uint64_t latestConstrainedSequence) noexcept {
    return appliedSequence >= latestConstrainedSequence;
}

bool ShouldUseConstrainedPollInterval(ResourcePressureValues const& values, bool probesComplete) noexcept {
    return probesComplete && values.IsBackgroundConstrained();
}

MemoryNotificationWaitPlan PlanMemoryNotificationWaits(std::optional<bool> lowMemorySignaled,
                                                       std::optional<bool> highMemorySignaled) noexcept {
    if (!lowMemorySignaled || !highMemorySignaled || (*lowMemorySignaled && *highMemorySignaled)) return {};
    return {
        .ArmLow = !*lowMemorySignaled,
        .ArmHigh = !*highMemorySignaled,
    };
}
