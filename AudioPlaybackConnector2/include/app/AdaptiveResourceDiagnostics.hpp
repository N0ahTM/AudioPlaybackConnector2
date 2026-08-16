#pragma once

#include <app/AdaptiveResourcePolicy.hpp>
#include <app/ResourcePressureState.hpp>

#include <string_view>

struct AdaptiveResourceDiagnostics {
    bool Evaluated = false;
    ResidencyPolicy Residency = ResidencyPolicy::Warm;
    ResidencyPolicy BackgroundResidency = ResidencyPolicy::Warm;
    ResourcePressureValues Pressure;
    bool SnapshotFresh = false;
    bool PositiveAuthorizationCurrent = false;
    bool PreloadAllowed = false;
    bool UiResourcesLoaded = false;
    bool UiResourcesInitialized = false;
};

[[nodiscard]] constexpr std::wstring_view ResidencyPolicyName(ResidencyPolicy value) noexcept {
    switch (value) {
        case ResidencyPolicy::Cold: return L"Cold";
        case ResidencyPolicy::Warm: return L"Warm";
        case ResidencyPolicy::Hot: return L"Hot";
    }
    return L"Unknown";
}

[[nodiscard]] constexpr std::wstring_view MemoryPressureStateName(MemoryPressureState value) noexcept {
    switch (value) {
        case MemoryPressureState::Unknown: return L"Unknown";
        case MemoryPressureState::Low: return L"Low";
        case MemoryPressureState::Neutral: return L"Neutral";
        case MemoryPressureState::High: return L"High";
    }
    return L"Unknown";
}

[[nodiscard]] constexpr std::wstring_view UserActivityStateName(UserActivityState value) noexcept {
    switch (value) {
        case UserActivityState::Unknown: return L"Unknown";
        case UserActivityState::Available: return L"Available";
        case UserActivityState::NotPresent: return L"NotPresent";
        case UserActivityState::Busy: return L"Busy";
        case UserActivityState::Fullscreen: return L"Fullscreen";
        case UserActivityState::Presentation: return L"Presentation";
        case UserActivityState::QuietTime: return L"QuietTime";
        case UserActivityState::ImmersiveApp: return L"ImmersiveApp";
    }
    return L"Unknown";
}
