#include <app/AdaptiveResourceDiagnostics.hpp>

#include <iostream>

namespace {

int g_failures = 0;

void Check(bool condition, char const* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

} // namespace

int RunAdaptiveResourceDiagnosticsTests() {
    Check(!AdaptiveResourceDiagnostics{}.Evaluated, "default diagnostics must not impersonate an evaluation");
    Check(ResidencyPolicyName(ResidencyPolicy::Cold) == L"Cold", "cold residency name must be stable");
    Check(ResidencyPolicyName(ResidencyPolicy::Warm) == L"Warm", "warm residency name must be stable");
    Check(ResidencyPolicyName(ResidencyPolicy::Hot) == L"Hot", "hot residency name must be stable");
    Check(MemoryPressureStateName(MemoryPressureState::High) == L"High", "memory-pressure name must be stable");
    Check(UserActivityStateName(UserActivityState::Fullscreen) == L"Fullscreen", "user-activity name must be stable");
    Check(UserActivityStateName(UserActivityState::ImmersiveApp) == L"ImmersiveApp",
          "immersive-app name must be stable");
    return g_failures;
}
