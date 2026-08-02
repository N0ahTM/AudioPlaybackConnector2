#include <app/ResourcePressureState.hpp>

bool ResourcePressureValues::IsMemoryPressure() const noexcept {
    return Memory == MemoryPressureState::Low;
}

bool ResourcePressureValues::HasReliableMemoryState() const noexcept {
    return Memory != MemoryPressureState::Unknown;
}

bool ResourcePressureValues::IsBackgroundConstrained() const noexcept {
    return !HasReliableMemoryState() || EnergySaver.value_or(true) || UserActivity == UserActivityState::Unknown ||
           UserActivity == UserActivityState::NotPresent || UserActivity == UserActivityState::Busy ||
           UserActivity == UserActivityState::Fullscreen || UserActivity == UserActivityState::Presentation ||
           UserActivity == UserActivityState::ImmersiveApp;
}

bool ResourcePressureValues::CanPreload() const noexcept {
    return Memory == MemoryPressureState::High && EnergySaver == false && UserActivity == UserActivityState::Available;
}

ResourcePressureValues ResourcePressureStateReducer::Apply(ResourcePressureProbe const& probe) noexcept {
    if (probe.LowMemorySignaled == true) {
        m_current.Memory = MemoryPressureState::Low;
    } else if (probe.HighMemorySignaled == true) {
        m_current.Memory = MemoryPressureState::High;
    } else if (probe.LowMemorySignaled && probe.HighMemorySignaled) {
        m_current.Memory = MemoryPressureState::Neutral;
    } else if ((probe.LowMemorySignaled == false &&
                (m_current.Memory == MemoryPressureState::Unknown || m_current.Memory == MemoryPressureState::Low)) ||
               (probe.HighMemorySignaled == false && m_current.Memory == MemoryPressureState::High)) {
        m_current.Memory = MemoryPressureState::Neutral;
    }

    if (probe.UserActivity) {
        m_current.UserActivity = *probe.UserActivity;
    }
    if (probe.EnergySaver) {
        m_current.EnergySaver = *probe.EnergySaver;
    }
    return m_current;
}

ResourcePressureValues const& ResourcePressureStateReducer::Current() const noexcept {
    return m_current;
}
