#include "TestCheck.hpp"

#include <app/SingleInstanceGuard.hpp>

#include <iostream>
#include <string_view>

namespace {

void TestSingleInstanceGuardOwnershipAndIdempotence() {
    auto const baseName = std::wstring(L"AudioPlaybackConnector2_CoreTests_") + std::to_wstring(GetCurrentProcessId()) +
                          L"_" + std::to_wstring(GetTickCount64());
    auto const otherName = baseName + L"_other";

    {
        SingleInstanceGuard first;
        SingleInstanceGuard second;
        Check(first.TryAcquire(baseName), "the first guard must acquire a unique mutex");
        Check(first.TryAcquire(baseName), "reacquiring the same name on one guard must be idempotent");
        Check(!first.TryAcquire(otherName), "one guard must never silently replace its held mutex");
        Check(!second.TryAcquire(baseName), "a second guard must observe the held mutex");
    }

    SingleInstanceGuard third;
    Check(third.TryAcquire(baseName), "destruction of the owner must make the mutex acquirable again");
    Check(!third.TryAcquire(otherName), "a guard holding a mutex must not switch to another name");
}

} // namespace

int RunSingleInstanceGuardTests() {
    TestSingleInstanceGuardOwnershipAndIdempotence();
    return g_failures;
}
