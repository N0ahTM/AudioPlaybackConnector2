#include <app/AdaptiveResourcePolicy.hpp>

#include <chrono>
#include <iostream>
#include <string_view>

namespace {
using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

AdaptiveResourcePolicyConfig TestConfig() {
    return {
        .BackgroundPressureToColdDelay = 5s,
        .ColdToWarmDelay = 30s,
        .WarmToHotDelay = 20s,
        .InteractionHotHold = 10s,
    };
}

AdaptiveResourcePolicy::TimePoint At(std::chrono::seconds elapsed) {
    return AdaptiveResourcePolicy::TimePoint{} + elapsed;
}

void TestWarmupAndActions() {
    AdaptiveResourcePolicy policy(TestConfig());
    AdaptiveResourcePolicyInput input;

    auto initial = policy.Evaluate(input, At(0s));
    Check(initial.Residency == ResidencyPolicy::Warm, "startup must begin warm without preloading heavy UI");
    Check(initial.Action == AdaptiveResourceAction::None, "warm startup must not request an action");

    auto beforeHot = policy.Evaluate(input, At(19s));
    Check(beforeHot.Residency == ResidencyPolicy::Warm, "warm-to-hot promotion must honor its full delay");

    auto hot = policy.Evaluate(input, At(20s));
    Check(hot.Residency == ResidencyPolicy::Hot, "healthy warm state must promote to hot at the threshold");
    Check(hot.Action == AdaptiveResourceAction::PreloadUi, "entering hot must request missing UI resources");
    Check(hot.ResidencyChanged && hot.BackgroundResidencyChanged, "hot promotion must report both transitions");

    auto retry = policy.Evaluate(input, At(21s));
    Check(retry.Action == AdaptiveResourceAction::PreloadUi, "failed or pending preloads must be requested again");
    input.UiResourcesLoaded = true;
    Check(policy.Evaluate(input, At(22s)).Action == AdaptiveResourceAction::None,
          "loaded hot resources must not be preloaded twice");
}

void TestImmediateMemoryPressureAndStagedRecovery() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput input{.UiResourcesLoaded = true};
    static_cast<void>(policy.Evaluate(input, At(0s)));

    input.MemoryPressure = true;
    auto cold = policy.Evaluate(input, At(1s));
    Check(cold.Residency == ResidencyPolicy::Cold, "memory pressure must bypass the background-pressure grace period");
    Check(cold.Action == AdaptiveResourceAction::ReleaseUi, "cold state must release loaded heavy UI");

    input.MemoryPressure = false;
    input.UiResourcesLoaded = false;
    Check(policy.Evaluate(input, At(2s)).Residency == ResidencyPolicy::Cold,
          "recovery must start only after pressure has cleared");
    Check(policy.Evaluate(input, At(31s)).Residency == ResidencyPolicy::Cold,
          "cold-to-warm recovery must honor its full delay");

    auto warm = policy.Evaluate(input, At(32s));
    Check(warm.Residency == ResidencyPolicy::Warm, "healthy cold state must recover to warm at the threshold");
    Check(warm.Action == AdaptiveResourceAction::None, "warm recovery must not create heavy UI");
    Check(policy.Evaluate(input, At(51s)).Residency == ResidencyPolicy::Warm,
          "warm-to-hot recovery must use an independent second delay");
    auto hot = policy.Evaluate(input, At(52s));
    Check(hot.Residency == ResidencyPolicy::Hot, "staged recovery must eventually return to hot");
    Check(hot.Action == AdaptiveResourceAction::PreloadUi, "completed recovery must request preloading");
}

void TestBackgroundPressureGraceAndHysteresis() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput input{.UiResourcesLoaded = true};
    static_cast<void>(policy.Evaluate(input, At(0s)));

    input.FullscreenOrPresentation = true;
    Check(policy.Evaluate(input, At(1s)).Residency == ResidencyPolicy::Hot,
          "brief fullscreen activity must not evict hot resources");
    Check(policy.Evaluate(input, At(5s)).Residency == ResidencyPolicy::Hot,
          "fullscreen pressure must remain hot before the exact grace threshold");
    Check(policy.Evaluate(input, At(6s)).Residency == ResidencyPolicy::Cold,
          "sustained fullscreen pressure must transition to cold");

    AdaptiveResourcePolicy transientPolicy(TestConfig(), ResidencyPolicy::Warm);
    AdaptiveResourcePolicyInput transientInput;
    static_cast<void>(transientPolicy.Evaluate(transientInput, At(0s)));
    transientInput.EnergySaver = true;
    static_cast<void>(transientPolicy.Evaluate(transientInput, At(15s)));
    transientInput.EnergySaver = false;
    Check(transientPolicy.Evaluate(transientInput, At(18s)).Residency == ResidencyPolicy::Warm,
          "transient pressure must not promote or demote warm state");
    Check(transientPolicy.Evaluate(transientInput, At(37s)).Residency == ResidencyPolicy::Warm,
          "ending transient pressure must restart the hot stability window");
    Check(transientPolicy.Evaluate(transientInput, At(38s)).Residency == ResidencyPolicy::Hot,
          "warm state may promote after a complete post-pressure window");
}

void TestVisibleUiPinsAColdBackgroundDecision() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput input{.UiVisible = true, .UiResourcesLoaded = true};
    static_cast<void>(policy.Evaluate(input, At(0s)));

    input.MemoryPressure = true;
    auto pinned = policy.Evaluate(input, At(1s));
    Check(pinned.BackgroundResidency == ResidencyPolicy::Cold, "memory pressure must remain observable while pinned");
    Check(pinned.Residency == ResidencyPolicy::Hot, "visible UI must remain hot while its release is pinned");
    Check(pinned.Pinned && pinned.ReleaseDeferred, "visible UI must report its deferred cold transition");
    Check(pinned.Action == AdaptiveResourceAction::None, "visible UI must never be released underneath the user");

    input.UiVisible = false;
    input.UiPinned = true;
    auto transitioning = policy.Evaluate(input, At(2s));
    Check(transitioning.Residency == ResidencyPolicy::Hot && transitioning.Pinned,
          "explicit pinning must protect UI during non-visible opening or closing transitions");
    Check(transitioning.Action == AdaptiveResourceAction::None,
          "explicit pinning must defer release until the transition has finished");

    input.UiPinned = false;
    auto unpinned = policy.Evaluate(input, At(3s));
    Check(unpinned.Residency == ResidencyPolicy::Cold, "closing UI must apply the pending cold state immediately");
    Check(unpinned.Action == AdaptiveResourceAction::ReleaseUi, "closing pinned UI must release its resources");
    Check(!unpinned.Pinned && !unpinned.ReleaseDeferred, "closed UI must clear pin metadata");
}

void TestInteractionTemporarilyOverridesCold() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Cold);
    AdaptiveResourcePolicyInput input{.MemoryPressure = true, .UserInteraction = true};

    auto requested = policy.Evaluate(input, At(0s));
    Check(requested.BackgroundResidency == ResidencyPolicy::Cold, "interaction must not hide background pressure");
    Check(requested.Residency == ResidencyPolicy::Hot, "user interaction must make UI available even while cold");
    Check(requested.Action == AdaptiveResourceAction::PreloadUi, "cold interaction must request UI on demand");
    Check(!requested.Pinned && requested.ReleaseDeferred, "interaction hold is distinct from visible pinning");

    input.UserInteraction = false;
    input.UiResourcesLoaded = true;
    Check(policy.Evaluate(input, At(9s)).Residency == ResidencyPolicy::Hot,
          "interaction hot hold must survive until its configured deadline");
    auto expired = policy.Evaluate(input, At(10s));
    Check(expired.Residency == ResidencyPolicy::Cold, "interaction hot hold must expire exactly at its deadline");
    Check(expired.Action == AdaptiveResourceAction::ReleaseUi, "expired cold interaction must release loaded UI");
}

void TestClockRollbackRestartsStabilityWindows() {
    AdaptiveResourcePolicy policy(TestConfig());
    AdaptiveResourcePolicyInput input;
    static_cast<void>(policy.Evaluate(input, At(100s)));
    static_cast<void>(policy.Evaluate(input, At(115s)));

    auto rolledBack = policy.Evaluate(input, At(90s));
    Check(rolledBack.Residency == ResidencyPolicy::Warm, "clock rollback must not cause an early transition");
    Check(policy.Evaluate(input, At(109s)).Residency == ResidencyPolicy::Warm,
          "clock rollback must restart the entire stability window");
    Check(policy.Evaluate(input, At(110s)).Residency == ResidencyPolicy::Hot,
          "policy must recover deterministically after clock rollback");
}

void TestNegativeDurationsAreClamped() {
    AdaptiveResourcePolicyConfig config{
        .BackgroundPressureToColdDelay = -1ms,
        .ColdToWarmDelay = -1ms,
        .WarmToHotDelay = -1ms,
        .InteractionHotHold = -1ms,
    };
    AdaptiveResourcePolicy policy(config, ResidencyPolicy::Cold);
    AdaptiveResourcePolicyInput input;
    Check(policy.Evaluate(input, At(0s)).Residency == ResidencyPolicy::Warm,
          "negative cold recovery delay must be clamped to zero");
    Check(policy.Evaluate(input, At(0s)).Residency == ResidencyPolicy::Hot,
          "zero delays must still produce deterministic staged transitions");

    input.UserInteraction = true;
    input.MemoryPressure = true;
    auto interaction = policy.Evaluate(input, At(1s));
    Check(interaction.Residency == ResidencyPolicy::Hot, "zero-duration interaction must remain valid for its event");
    input.UserInteraction = false;
    Check(policy.Evaluate(input, At(1s)).Residency == ResidencyPolicy::Cold,
          "zero-duration interaction hold must not outlive its event");
}
} // namespace

int RunAdaptiveResourcePolicyTests() {
    TestWarmupAndActions();
    TestImmediateMemoryPressureAndStagedRecovery();
    TestBackgroundPressureGraceAndHysteresis();
    TestVisibleUiPinsAColdBackgroundDecision();
    TestInteractionTemporarilyOverridesCold();
    TestClockRollbackRestartsStabilityWindows();
    TestNegativeDurationsAreClamped();
    return g_failures;
}
