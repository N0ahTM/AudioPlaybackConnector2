#include <app/AdaptiveResourcePolicy.hpp>

#include <algorithm>

namespace {
std::chrono::milliseconds NonNegative(std::chrono::milliseconds value) noexcept {
    return std::max(value, std::chrono::milliseconds::zero());
}

bool ElapsedAtLeast(AdaptiveResourcePolicy::TimePoint now,
                    AdaptiveResourcePolicy::TimePoint since,
                    std::chrono::milliseconds duration) noexcept {
    return duration <= std::chrono::milliseconds::zero() || now - since >= duration;
}

AdaptiveResourcePolicy::TimePoint SaturatingAdd(AdaptiveResourcePolicy::TimePoint now,
                                                std::chrono::milliseconds duration) noexcept {
    const auto converted = std::chrono::duration_cast<AdaptiveResourcePolicy::Clock::duration>(duration);
    if (converted >= AdaptiveResourcePolicy::TimePoint::max() - now) {
        return AdaptiveResourcePolicy::TimePoint::max();
    }
    return now + converted;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AdaptiveResourcePolicy::AdaptiveResourcePolicy(AdaptiveResourcePolicyConfig config,
                                               ResidencyPolicy initialResidency) noexcept
    : m_config(config), m_backgroundResidency(initialResidency), m_effectiveResidency(initialResidency) {
    m_config.BackgroundPressureToColdDelay = NonNegative(m_config.BackgroundPressureToColdDelay);
    m_config.ColdToWarmDelay = NonNegative(m_config.ColdToWarmDelay);
    m_config.WarmToHotDelay = NonNegative(m_config.WarmToHotDelay);
    m_config.InteractionHotHold = NonNegative(m_config.InteractionHotHold);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

AdaptiveResourcePolicyDecision AdaptiveResourcePolicy::Evaluate(AdaptiveResourcePolicyInput const& input,
                                                                TimePoint now) noexcept {
    const bool pressureActive = input.MemoryPressure || input.FullscreenOrPresentation || input.EnergySaver;
    if (!m_lastEvaluation) {
        Initialize(now, pressureActive);
    } else if (now < *m_lastEvaluation) {
        ResetTemporalStateAfterClockRollback(now, pressureActive);
    }
    m_lastEvaluation = now;

    const auto previousBackgroundResidency = m_backgroundResidency;
    UpdateBackgroundResidency(input, now);

    if (input.UserInteraction) {
        m_interactionUntil = SaturatingAdd(now, m_config.InteractionHotHold);
    }
    const bool interactionHeld = input.UserInteraction || (m_interactionUntil && now < *m_interactionUntil);
    if (!interactionHeld) {
        m_interactionUntil.reset();
    }

    const bool pinned = input.UiVisible || input.UiPinned;
    const bool foregroundDemand = pinned || interactionHeld;
    const auto effectiveResidency = foregroundDemand ? ResidencyPolicy::Hot : m_backgroundResidency;
    const auto previousEffectiveResidency = m_effectiveResidency;
    m_effectiveResidency = effectiveResidency;

    AdaptiveResourceAction action = AdaptiveResourceAction::None;
    if (effectiveResidency == ResidencyPolicy::Hot && !input.UiResourcesLoaded) {
        if (foregroundDemand || (input.PreloadAllowed && !pressureActive)) {
            action = AdaptiveResourceAction::PreloadUi;
        }
    } else if (effectiveResidency != ResidencyPolicy::Hot && input.UiResourcesLoaded) {
        action = AdaptiveResourceAction::ReleaseUi;
    }

    return {
        .PreviousResidency = previousEffectiveResidency,
        .Residency = effectiveResidency,
        .BackgroundResidency = m_backgroundResidency,
        .Action = action,
        .ResidencyChanged = effectiveResidency != previousEffectiveResidency,
        .BackgroundResidencyChanged = m_backgroundResidency != previousBackgroundResidency,
        .Pinned = pinned,
        .ReleaseDeferred = foregroundDemand && m_backgroundResidency != ResidencyPolicy::Hot,
    };
}

ResidencyPolicy AdaptiveResourcePolicy::BackgroundResidency() const noexcept {
    return m_backgroundResidency;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void AdaptiveResourcePolicy::Initialize(TimePoint now, bool pressureActive) noexcept {
    if (pressureActive) {
        m_pressureSince = now;
    } else {
        m_healthySince = now;
    }
}

void AdaptiveResourcePolicy::ResetTemporalStateAfterClockRollback(TimePoint now, bool pressureActive) noexcept {
    m_pressureSince.reset();
    m_healthySince.reset();
    m_preloadAllowedSince.reset();
    m_interactionUntil.reset();
    if (pressureActive) {
        m_pressureSince = now;
    } else {
        m_healthySince = now;
    }
}

void AdaptiveResourcePolicy::SetBackgroundResidency(ResidencyPolicy residency) noexcept {
    if (m_backgroundResidency == residency) return;
    m_backgroundResidency = residency;
    m_preloadAllowedSince.reset();
}

void AdaptiveResourcePolicy::UpdateBackgroundResidency(AdaptiveResourcePolicyInput const& input,
                                                       TimePoint now) noexcept {
    const bool pressureActive = input.MemoryPressure || input.FullscreenOrPresentation || input.EnergySaver;
    if (pressureActive) {
        m_healthySince.reset();
        m_preloadAllowedSince.reset();
        if (!m_pressureSince) {
            m_pressureSince = now;
        }

        if (input.MemoryPressure || ElapsedAtLeast(now, *m_pressureSince, m_config.BackgroundPressureToColdDelay)) {
            SetBackgroundResidency(ResidencyPolicy::Cold);
        }
        return;
    }

    if (m_pressureSince) {
        m_pressureSince.reset();
        m_healthySince = now;
        m_preloadAllowedSince.reset();
    } else if (!m_healthySince) {
        m_healthySince = now;
    }

    if (m_backgroundResidency == ResidencyPolicy::Cold) {
        if (ElapsedAtLeast(now, *m_healthySince, m_config.ColdToWarmDelay)) {
            SetBackgroundResidency(ResidencyPolicy::Warm);
            if (input.PreloadAllowed) {
                m_preloadAllowedSince = now;
            }
        }
        return;
    }

    if (m_backgroundResidency == ResidencyPolicy::Hot) {
        if (!input.PreloadAllowed && !input.UiResourcesLoaded) {
            SetBackgroundResidency(ResidencyPolicy::Warm);
        }
        return;
    }

    if (!input.PreloadAllowed) {
        m_preloadAllowedSince.reset();
        return;
    }

    if (!m_preloadAllowedSince) {
        m_preloadAllowedSince = now;
    }
    if (ElapsedAtLeast(now, *m_preloadAllowedSince, m_config.WarmToHotDelay)) {
        SetBackgroundResidency(ResidencyPolicy::Hot);
    }
}
