#include <util/RuntimeApartment.hpp>

#include <roapi.h>
#include <winstring.h>

#include <atomic>
#include <iostream>
#include <iterator>
#include <string_view>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestFreshWorkerCanActivateWindowsRuntimeClass() {
    std::atomic<bool> ready = false;
    std::atomic<HRESULT> activationResult = E_UNEXPECTED;

    std::jthread worker([&]() {
        util::RuntimeApartment apartment;
        ready.store(apartment.Ready());
        if (!apartment.Ready()) {
            activationResult.store(apartment.Result());
            return;
        }

        HSTRING_HEADER classNameHeader{};
        HSTRING className = nullptr;
        constexpr wchar_t c_jsonObjectClass[] = L"Windows.Data.Json.JsonObject";
        auto result = WindowsCreateStringReference(
            c_jsonObjectClass, static_cast<UINT32>(std::size(c_jsonObjectClass) - 1), &classNameHeader, &className);
        if (FAILED(result)) {
            activationResult.store(result);
            return;
        }

        IInspectable* instance = nullptr;
        result = RoActivateInstance(className, &instance);
        if (instance) instance->Release();
        activationResult.store(result);
    });
    worker.join();

    Check(ready.load(), "a fresh worker thread must initialize a usable Windows Runtime apartment");
    Check(SUCCEEDED(activationResult.load()),
          "a RuntimeApartment worker must activate the same Windows.Data.Json class used by SettingsStore");
}

} // namespace

int RunRuntimeApartmentTests() {
    TestFreshWorkerCanActivateWindowsRuntimeClass();
    return g_failures;
}
