#include "TestCheck.hpp"

#include <app/AdaptiveActionRetryBackoff.hpp>

#include <chrono>

namespace {
using namespace std::chrono_literals;

void TestBoundedAndResettableRetry() {
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
} // namespace

int RunAdaptiveActionRetryBackoffTests() {
    TestBoundedAndResettableRetry();
    return g_failures;
}
