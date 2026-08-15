#pragma once

#include <app/AdaptiveActionRetryBackoff.hpp>
#include <app/AdaptiveScheduleState.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Adaptive Resource Policy //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class ResidencyPolicy { Cold, Warm, Hot };

enum class AdaptiveResourceAction { None, PreloadUi, ReleaseUi };

struct AdaptiveResourcePolicyConfig {
    std::chrono::milliseconds BackgroundPressureToColdDelay{std::chrono::seconds{5}};
    std::chrono::milliseconds ColdToWarmDelay{std::chrono::seconds{45}};
    std::chrono::milliseconds WarmToHotDelay{std::chrono::seconds{20}};
    std::chrono::milliseconds InteractionHotHold{std::chrono::seconds{10}};
};

struct AdaptiveResourcePolicyInput {
    bool MemoryPressure = false;
    bool PreloadAllowed = false;
    bool FullscreenOrPresentation = false;
    bool EnergySaver = false;
    bool UiVisible = false;
    bool UiPinned = false;
    bool UserInteraction = false;
    bool UiResourcesLoaded = false;
    bool UiResourcesInitialized = false;
};

struct AdaptiveResourcePolicyDecision {
    ResidencyPolicy PreviousResidency = ResidencyPolicy::Warm;
    ResidencyPolicy Residency = ResidencyPolicy::Warm;
    ResidencyPolicy BackgroundResidency = ResidencyPolicy::Warm;
    AdaptiveResourceAction Action = AdaptiveResourceAction::None;
    bool ResidencyChanged = false;
    bool BackgroundResidencyChanged = false;
    bool Pinned = false;
    bool ReleaseDeferred = false;
    std::optional<std::chrono::steady_clock::time_point> ReevaluateAt;
};

class AdaptiveResourcePolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit AdaptiveResourcePolicy(AdaptiveResourcePolicyConfig config = {},
                                    ResidencyPolicy initialResidency = ResidencyPolicy::Warm) noexcept;

    [[nodiscard]] AdaptiveResourcePolicyDecision Evaluate(AdaptiveResourcePolicyInput const& input,
                                                          TimePoint now) noexcept;
    [[nodiscard]] ResidencyPolicy BackgroundResidency() const noexcept;

private:
    void Initialize(TimePoint now, bool pressureActive) noexcept;
    void ResetTemporalStateAfterClockRollback(TimePoint now, bool pressureActive) noexcept;
    void SetBackgroundResidency(ResidencyPolicy residency) noexcept;
    void UpdateBackgroundResidency(AdaptiveResourcePolicyInput const& input, TimePoint now) noexcept;
    [[nodiscard]] std::optional<TimePoint>
    NextReevaluation(AdaptiveResourcePolicyInput const& input, TimePoint now, bool foregroundDemand) const noexcept;

    AdaptiveResourcePolicyConfig m_config;
    ResidencyPolicy m_backgroundResidency = ResidencyPolicy::Warm;
    ResidencyPolicy m_effectiveResidency = ResidencyPolicy::Warm;
    std::optional<TimePoint> m_pressureSince;
    std::optional<TimePoint> m_healthySince;
    std::optional<TimePoint> m_preloadAllowedSince;
    std::optional<TimePoint> m_interactionUntil;
    std::optional<TimePoint> m_lastEvaluation;
};
