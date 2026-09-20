#include "TestCheck.hpp"
#include <app/UiDispatcher.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <semaphore>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <wil/resource.h>

namespace {
wil::unique_hwnd MakeWindow() {
    auto window = wil::unique_hwnd(CreateWindowExW(
        0, L"STATIC", L"UiDispatcherTests", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr));
    if (!window) throw std::runtime_error("test message window creation failed");
    return window;
}

std::size_t Pump(HWND window, UiDispatcher& dispatcher) {
    std::size_t handled = 0;
    MSG message{};
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
        if (dispatcher.HandleMessage(message.message))
            ++handled;
        else
            DispatchMessageW(&message);
    }
    return handled;
}

void TestInlineAndPrimaryDispatch() {
    auto window = MakeWindow();
    std::vector<UiDispatcher::Task> posted;
    UiDispatcher dispatcher(window.get(), [&](auto task) {
        posted.push_back(std::move(task));
        return true;
    });
    int calls = 0;
    Check(dispatcher.Run([&] { ++calls; }) && calls == 1 && posted.empty(), "UI work must run inline");
    Check(!dispatcher.Run([] { throw std::runtime_error("expected"); }),
          "inline exceptions must be reported as failure");
    std::atomic_bool accepted = false;
    DWORD callbackThread = 0;
    std::jthread producer([&] {
        accepted = dispatcher.Run([&] {
            ++calls;
            callbackThread = GetCurrentThreadId();
        });
    });
    producer.join();
    Check(accepted && calls == 1 && posted.size() == 1, "worker work must await the UI dispatcher");
    if (!posted.empty()) posted.front()();
    Check(calls == 2 && callbackThread == GetCurrentThreadId(), "queued work must execute on UI");
}

void TestConcurrentNativeFallbackAndExceptions() {
    auto window = MakeWindow();
    UiDispatcher dispatcher(window.get(), [](auto) { return false; });
    constexpr int count = 16;
    std::barrier start(count);
    std::atomic<int> accepted = 0;
    std::vector<std::jthread> producers;
    int calls = 0;
    bool allOnUi = true;
    const auto uiThread = GetCurrentThreadId();
    for (int index = 0; index < count; ++index) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            if (dispatcher.Run([&] {
                    ++calls;
                    allOnUi = allOnUi && GetCurrentThreadId() == uiThread;
                }))
                ++accepted;
        });
    }
    producers.clear();
    Check(accepted == count && calls == 0, "native posts must return without waiting for or invoking UI");
    std::jthread extra([&] {
        Check(dispatcher.Run([] { throw std::runtime_error("expected"); }), "throwing work can still be queued");
        Check(dispatcher.Run([&] { ++calls; }), "one throwing callback must not prevent later work");
    });
    extra.join();
    Pump(window.get(), dispatcher);
    Check(calls == count + 1 && allOnUi, "all accepted fallback tasks must execute exactly once on UI");
    Pump(window.get(), dispatcher);
    Check(calls == count + 1, "redundant wake messages must not repeat work");
}

void TestStopReleasesCallbacksOutsideQueueLock() {
    auto window = MakeWindow();
    UiDispatcher dispatcher(window.get(), [](auto) { return false; });
    bool released = false;
    bool rejected = false;
    auto data = std::shared_ptr<int>(new int(0), [&](int* value) {
        delete value;
        released = true;
        rejected = !dispatcher.Run([] {});
        dispatcher.Stop();
    });
    std::jthread producer([&, data = std::move(data)]() mutable {
        Check(dispatcher.Run([data = std::move(data)] {}), "task must be queued before stop");
    });
    producer.join();
    dispatcher.Stop();
    Check(released && rejected, "stop must release captures unlocked and reject reentrant work");
    Pump(window.get(), dispatcher);
}

void TestStopDrainsNativePostsBeforeWindowDestruction() {
    auto window = MakeWindow();
    UiDispatcher dispatcher(window.get(), [](auto) { return false; });
    constexpr int count = 8;
    std::barrier start(count + 1);
    std::binary_semaphore firstPost(0);
    std::atomic_bool signaled = false;
    std::vector<std::jthread> producers;
    int calls = 0;
    for (int index = 0; index < count; ++index) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (int attempt = 0; attempt < 64; ++attempt) {
                if (dispatcher.Run([&] { ++calls; }) && !signaled.exchange(true)) firstPost.release();
            }
        });
    }
    start.arrive_and_wait();
    Check(firstPost.try_acquire_for(std::chrono::seconds(2)), "a native post must be admitted before stop");
    dispatcher.Stop();
    static_cast<void>(Pump(window.get(), dispatcher));
    producers.clear();
    Check(Pump(window.get(), dispatcher) == 0, "no admitted native post may outlive Stop");
    Check(calls == 0 && !dispatcher.Run([] {}), "stop must discard queued work and reject future work");
}

void TestLatePrimaryCallbackDoesNotRetainOwner() {
    auto window = MakeWindow();
    UiDispatcher::Task posted;
    auto dispatcher = std::make_unique<UiDispatcher>(window.get(), [&](auto task) {
        posted = std::move(task);
        return true;
    });
    int calls = 0;
    std::jthread producer([&] { Check(dispatcher->Run([&] { ++calls; }), "primary task must be accepted"); });
    producer.join();
    dispatcher.reset();
    if (posted) posted();
    Check(calls == 0, "late dispatcher callbacks must not access a released owner");
}

void TestNativePostFailureIsRejected() {
    auto window = MakeWindow();
    UiDispatcher dispatcher(window.get(), [](auto) -> bool { throw std::runtime_error("dispatcher failed"); });
    window.reset(); // Deliberate platform failure: no other window is created before the post.
    bool accepted = true;
    int calls = 0;
    std::jthread producer([&] { accepted = dispatcher.Run([&] { ++calls; }); });
    producer.join();
    Check(!accepted && calls == 0, "failure of both transports must reject the task");
}
} // namespace

int RunUiDispatcherTests() {
    TestInlineAndPrimaryDispatch();
    TestConcurrentNativeFallbackAndExceptions();
    TestStopReleasesCallbacksOutsideQueueLock();
    TestStopDrainsNativePostsBeforeWindowDestruction();
    TestLatePrimaryCallbackDoesNotRetainOwner();
    TestNativePostFailureIsRejected();
    return g_failures;
}
