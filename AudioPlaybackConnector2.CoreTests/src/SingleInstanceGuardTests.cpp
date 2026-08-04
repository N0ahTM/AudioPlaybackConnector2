#include <app/SingleInstanceGuard.hpp>

#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestSingleInstanceGuardOwnershipAndIdempotence() {
    auto const baseName = std::wstring(L"AudioPlaybackConnector2_CoreTests_") + std::to_wstring(GetCurrentProcessId()) +
                          L"_" + std::to_wstring(GetTickCount64());
    auto const otherName = baseName + L"_other";

    SingleInstanceGuard first;
    SingleInstanceGuard second;
    Check(first.TryAcquire(baseName), "the first guard must acquire a unique mutex");
    Check(first.TryAcquire(baseName), "reacquiring the same name on one guard must be idempotent");
    Check(!first.TryAcquire(otherName), "one guard must never silently replace its held mutex");
    Check(!second.TryAcquire(baseName), "a second guard must observe the held mutex");

    first.Release();
    Check(second.TryAcquire(baseName), "releasing the owner must make the mutex acquirable again");
    second.Release();
    second.Release();
    Check(second.TryAcquire(otherName), "release must reset both handle and remembered name");
    second.Release();
}

} // namespace

int RunSingleInstanceGuardTests() {
    TestSingleInstanceGuardOwnershipAndIdempotence();
    return g_failures;
}
