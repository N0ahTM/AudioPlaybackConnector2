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
    AdaptiveResourcePolicyInput input{.PreloadAllowed = true};

    auto initial = policy.Evaluate(input, At(0s));
    Check(initial.Residency == ResidencyPolicy::Warm, "startup must begin warm without preloading heavy UI");
    Check(initial.Action == AdaptiveResourceAction::None, "warm startup must not request an action");
    Check(initial.ReevaluateAt == At(20s), "warm promotion must expose its exact one-shot reevaluation deadline");

    auto beforeHot = policy.Evaluate(input, At(19s));
    Check(beforeHot.Residency == ResidencyPolicy::Warm, "warm-to-hot promotion must honor its full delay");

    auto hot = policy.Evaluate(input, At(20s));
    Check(hot.Residency == ResidencyPolicy::Hot, "healthy warm state must promote to hot at the threshold");
    Check(hot.Action == AdaptiveResourceAction::PreloadUi, "entering hot must request missing UI resources");
    Check(hot.ResidencyChanged && hot.BackgroundResidencyChanged, "hot promotion must report both transitions");
    Check(!hot.ReevaluateAt, "stable hot state must not keep a background timer alive");

    auto retry = policy.Evaluate(input, At(21s));
    Check(retry.Action == AdaptiveResourceAction::PreloadUi, "failed or pending preloads must be requested again");
    input.UiResourcesLoaded = true;
    Check(policy.Evaluate(input, At(22s)).Action == AdaptiveResourceAction::PreloadUi,
          "a partially created UI must keep retrying until its data is ready");
    input.UiResourcesInitialized = true;
    Check(policy.Evaluate(input, At(22s)).Action == AdaptiveResourceAction::None,
          "loaded hot resources must not be preloaded twice");
}

void TestImmediateMemoryPressureAndStagedRecovery() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput input{
        .PreloadAllowed = true,
        .UiResourcesLoaded = true,
        .UiResourcesInitialized = true,
    };
    static_cast<void>(policy.Evaluate(input, At(0s)));

    input.MemoryPressure = true;
    auto cold = policy.Evaluate(input, At(1s));
    Check(cold.Residency == ResidencyPolicy::Cold, "memory pressure must bypass the background-pressure grace period");
    Check(cold.Action == AdaptiveResourceAction::ReleaseUi, "cold state must release loaded heavy UI");

    input.MemoryPressure = false;
    input.UiResourcesLoaded = false;
    auto recovering = policy.Evaluate(input, At(2s));
    Check(recovering.Residency == ResidencyPolicy::Cold, "recovery must start only after pressure has cleared");
    Check(recovering.ReevaluateAt == At(32s), "cold recovery must expose its exact one-shot deadline");
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
    AdaptiveResourcePolicyInput input{
        .PreloadAllowed = true,
        .UiResourcesLoaded = true,
        .UiResourcesInitialized = true,
    };
    static_cast<void>(policy.Evaluate(input, At(0s)));

    input.FullscreenOrPresentation = true;
    auto grace = policy.Evaluate(input, At(1s));
    Check(grace.Residency == ResidencyPolicy::Hot, "brief fullscreen activity must not evict hot resources");
    Check(grace.ReevaluateAt == At(6s), "background-pressure grace must schedule one exact reevaluation");
    Check(policy.Evaluate(input, At(5s)).Residency == ResidencyPolicy::Hot,
          "fullscreen pressure must remain hot before the exact grace threshold");
    Check(policy.Evaluate(input, At(6s)).Residency == ResidencyPolicy::Cold,
          "sustained fullscreen pressure must transition to cold");

    AdaptiveResourcePolicy transientPolicy(TestConfig(), ResidencyPolicy::Warm);
    AdaptiveResourcePolicyInput transientInput{.PreloadAllowed = true};
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

void TestNeutralMemoryMaintainsButNeverCreatesHotState() {
    AdaptiveResourcePolicy hotPolicy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput loadedHotInput{.UiResourcesLoaded = true, .UiResourcesInitialized = true};

    auto maintained = hotPolicy.Evaluate(loadedHotInput, At(0s));
    Check(maintained.Residency == ResidencyPolicy::Hot,
          "neutral memory state must retain already loaded hot resources");
    Check(maintained.Action == AdaptiveResourceAction::None,
          "neutral memory state must not disturb already loaded hot resources");
    Check(hotPolicy.Evaluate(loadedHotInput, At(60s)).Residency == ResidencyPolicy::Hot,
          "neutral memory state must not expire an existing hot residency");

    AdaptiveResourcePolicy warmPolicy(TestConfig());
    AdaptiveResourcePolicyInput neutralInput;
    static_cast<void>(warmPolicy.Evaluate(neutralInput, At(0s)));
    auto stillWarm = warmPolicy.Evaluate(neutralInput, At(60s));
    Check(stillWarm.Residency == ResidencyPolicy::Warm,
          "neutral memory state must never promote a warm background to hot");
    Check(stillWarm.Action == AdaptiveResourceAction::None,
          "neutral memory state must never request speculative preloading");

    AdaptiveResourcePolicy missingHotPolicy(TestConfig(), ResidencyPolicy::Hot);
    auto missingHot = missingHotPolicy.Evaluate(neutralInput, At(0s));
    Check(missingHot.BackgroundResidency == ResidencyPolicy::Warm,
          "neutral memory state must demote a hot intent whose resources are absent");
    Check(missingHot.Action == AdaptiveResourceAction::None,
          "neutral memory state must not recreate absent hot resources");

    AdaptiveResourcePolicy partialHotPolicy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput partialHotInput{.UiResourcesLoaded = true};
    auto partialHot = partialHotPolicy.Evaluate(partialHotInput, At(0s));
    Check(partialHot.BackgroundResidency == ResidencyPolicy::Warm,
          "neutral memory state must demote a hot intent whose initialization did not complete");
    Check(partialHot.Action == AdaptiveResourceAction::ReleaseUi,
          "a partially initialized view must be released when positive preload authorization is lost");
}

void TestPreloadPermissionMustRemainStableForFullPromotionDelay() {
    AdaptiveResourcePolicy policy(TestConfig());
    AdaptiveResourcePolicyInput input{.PreloadAllowed = true};

    static_cast<void>(policy.Evaluate(input, At(0s)));
    Check(policy.Evaluate(input, At(19s)).Residency == ResidencyPolicy::Warm,
          "preload permission must not promote before the full hot delay");

    input.PreloadAllowed = false;
    auto neutral = policy.Evaluate(input, At(20s));
    Check(neutral.Residency == ResidencyPolicy::Warm, "losing preload permission must cancel a pending hot promotion");
    Check(neutral.Action == AdaptiveResourceAction::None,
          "losing preload permission must not request speculative resources");
    Check(!neutral.ReevaluateAt,
          "losing preload permission must cancel the obsolete warm-to-hot reevaluation deadline");

    input.PreloadAllowed = true;
    static_cast<void>(policy.Evaluate(input, At(21s)));
    Check(policy.Evaluate(input, At(40s)).Residency == ResidencyPolicy::Warm,
          "renewed preload permission must restart the complete stability window");
    auto restabilized = policy.Evaluate(input, At(41s));
    Check(restabilized.Residency == ResidencyPolicy::Hot,
          "continuous high-memory permission may promote after complete restabilization");
    Check(restabilized.Action == AdaptiveResourceAction::PreloadUi,
          "completed high-memory restabilization must request preloading");
}

void TestVisibleUiPinsAColdBackgroundDecision() {
    AdaptiveResourcePolicy policy(TestConfig(), ResidencyPolicy::Hot);
    AdaptiveResourcePolicyInput input{.UiVisible = true, .UiResourcesLoaded = true, .UiResourcesInitialized = true};
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
    Check(requested.ReevaluateAt == At(10s), "interaction hold must expose its release deadline");

    input.UserInteraction = false;
    input.UiResourcesLoaded = true;
    input.UiResourcesInitialized = true;
    Check(policy.Evaluate(input, At(9s)).Residency == ResidencyPolicy::Hot,
          "interaction hot hold must survive until its configured deadline");
    auto expired = policy.Evaluate(input, At(10s));
    Check(expired.Residency == ResidencyPolicy::Cold, "interaction hot hold must expire exactly at its deadline");
    Check(expired.Action == AdaptiveResourceAction::ReleaseUi, "expired cold interaction must release loaded UI");
}

void TestClockRollbackRestartsStabilityWindows() {
    AdaptiveResourcePolicy policy(TestConfig());
    AdaptiveResourcePolicyInput input{.PreloadAllowed = true};
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
    AdaptiveResourcePolicyInput input{.PreloadAllowed = true};
    auto warm = policy.Evaluate(input, At(0s));
    Check(warm.Residency == ResidencyPolicy::Warm, "negative cold recovery delay must be clamped to zero");
    Check(warm.ReevaluateAt == At(0s), "a staged zero-delay transition must request one immediate reevaluation");
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

void TestAdaptiveActionRetryBackoffIsBoundedAndResettable() {
    AdaptiveActionRetryBackoff retry(1s, 30s);
    Check(retry.RecordFailure() == 1s, "first adaptive action retry must remain prompt");
    Check(retry.RecordFailure() == 2s, "adaptive action retries must back off exponentially");
    Check(retry.RecordFailure() == 4s, "adaptive action retry sequence must be deterministic");
    Check(retry.RecordFailure() == 8s, "adaptive action retry sequence must avoid a permanent 1 Hz loop");
    Check(retry.RecordFailure() == 16s, "adaptive action retry must approach its configured cap");
    Check(retry.RecordFailure() == 30s && retry.RecordFailure() == 30s,
          "adaptive action retry must remain bounded at its configured maximum");
    retry.Reset();
    Check(retry.CurrentDelay() == 1s && retry.RecordFailure() == 1s,
          "successful work or user interaction must restore the prompt retry delay");

    AdaptiveActionRetryBackoff clamped(-1s, -2s);
    Check(clamped.RecordFailure() == 1ms && clamped.CurrentDelay() == 1ms,
          "invalid retry configuration must clamp to a positive, non-growing delay");
}

void TestAdaptiveScheduleRejectsSupersededAndEarlyCallbacks() {
    AdaptiveScheduleState schedule;
    auto const first = schedule.Supersede();
    Check(schedule.SetWin32NotBefore(first, At(10s)), "the current schedule may arm its Win32 deadline");
    Check(!schedule.ConsumeWin32IfDue(At(9s)), "an early or stale WM_TIMER must not consume the active deadline");
    Check(schedule.ConsumeWin32IfDue(At(10s)), "the active Win32 deadline must be consumable exactly once");
    Check(!schedule.ConsumeWin32IfDue(At(11s)), "a duplicate WM_TIMER must be ignored after consumption");

    auto const superseded = schedule.Supersede();
    auto const current = schedule.Supersede();
    Check(current != superseded && schedule.Generation() == current,
          "each reschedule must receive a distinct current generation");
    Check(!schedule.Consume(superseded), "a queued dispatcher tick from an older schedule must be ignored");
    Check(schedule.Consume(current), "the current dispatcher tick must remain live");
    Check(!schedule.Consume(current), "a duplicate dispatcher tick must be rejected after consumption");

    auto const cancelled = schedule.Supersede();
    static_cast<void>(schedule.Supersede());
    Check(!schedule.Consume(cancelled), "cancelling a schedule must invalidate already queued callbacks");
}
} // namespace

int RunAdaptiveResourcePolicyTests() {
    TestWarmupAndActions();
    TestImmediateMemoryPressureAndStagedRecovery();
    TestBackgroundPressureGraceAndHysteresis();
    TestNeutralMemoryMaintainsButNeverCreatesHotState();
    TestPreloadPermissionMustRemainStableForFullPromotionDelay();
    TestVisibleUiPinsAColdBackgroundDecision();
    TestInteractionTemporarilyOverridesCold();
    TestClockRollbackRestartsStabilityWindows();
    TestNegativeDurationsAreClamped();
    TestAdaptiveActionRetryBackoffIsBoundedAndResettable();
    TestAdaptiveScheduleRejectsSupersededAndEarlyCallbacks();
    return g_failures;
}
