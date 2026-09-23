#include "TestCheck.hpp"
#include <core/StringResources.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <thread>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Resource Ownership and Language Publication ////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunStringResourcesTests() {
    const auto module = GetModuleHandleW(nullptr);
    auto owner = std::make_shared<StringResources>();
    std::shared_ptr<StringResources const> reader = owner;
    owner->Initialize(module, L"en", {});
    Check(reader->Get("Settings_Title") == L"Settings", "reader uses its explicit resource owner");
    Check(reader->Get("missing-key").empty(), "unknown key has no invented translation");
    owner->Initialize(module, L"de", {});
    const auto german = reader->Get("Settings_Title");
    Check(!german.empty() && german != L"Settings", "language replacement publishes translated resources");
    owner->Initialize(GetModuleHandleW(L"kernel32.dll"), L"en", {});
    Check(reader->Get("Settings_Title") == german, "missing baseline retains previous complete language");
    owner->Initialize(module, L"unknown-language", {});
    Check(reader->Get("Settings_Title") == L"Settings", "unsupported language selects English baseline");
    for (auto language : {L"en", L"de", L"fr", L"es", L"ja", L"ko", L"zh_hans", L"zh_hant"}) {
        owner->Initialize(module, language, {});
        Check(!reader->Get("Settings_Title").empty(), "embedded locale provides translated settings title");
        Check(!reader->Get("Privacy_RedactedDevice").empty(), "embedded locale provides privacy label");
    }
    owner->Initialize(module, L"en", {});
    std::barrier start(5);
    std::atomic_bool invalid = false;
    std::array<std::jthread, 4> readers;
    for (auto& thread : readers) {
        thread = std::jthread([&] {
            start.arrive_and_wait();
            for (int index = 0; index < 2000; ++index) {
                const auto title = reader->Get("Settings_Title");
                if (title != L"Settings" && title != german) invalid.store(true);
            }
        });
    }
    start.arrive_and_wait();
    for (int index = 0; index < 100; ++index)
        owner->Initialize(module, index % 2 ? L"de" : L"en", {});
    for (auto& thread : readers)
        thread.join();
    Check(!invalid.load(), "parallel reads see complete published values during language replacement");
    owner.reset();
    Check(reader->Get("Settings_Title") == german, "reader lifetime retains resources after host ownership ends");
    StringResources independent;
    independent.Initialize(module, L"en", {});
    Check(reader->Get("Settings_Title") == german, "independent resource owner does not change existing readers");
    return g_failures.load();
}
