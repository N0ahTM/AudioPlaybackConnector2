#include "TestCheck.hpp"
#include <app/UiDispatcher.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <semaphore>
#include <memory>
#include <future>
#include <array>
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
void TestActionInlineAdmissionAndResults() {
    auto window = MakeWindow();
    UiDispatcher dispatcher(window.get(), [](auto) { return false; });
    using Result = UiDispatcher::ActionResult;
    using Clock = std::chrono::steady_clock;
    auto forever = Clock::time_point::max();
    Check(dispatcher.RunAndWait([] { return true; }, {}, forever) == Result::Succeeded,
          "successful inline action must succeed");
    Check(dispatcher.RunAndWait([] { return false; }, {}, forever) == Result::Failed, "false inline action must fail");
    Check(dispatcher.RunAndWait([]() -> bool { throw std::runtime_error("expected"); }, {}, forever) == Result::Failed,
          "throwing inline action must fail");
    int calls = 0;
    auto work = [&] {
        ++calls;
        return true;
    };
    std::stop_source stop;
    stop.request_stop();
    Check(dispatcher.RunAndWait(work, stop.get_token(), forever) == Result::Failed,
          "cancelled inline action must be rejected");
    Check(dispatcher.RunAndWait(work, {}, Clock::now()) == Result::Failed, "expired inline action must be rejected");
    Check(calls == 0, "rejected inline actions must not mutate UI");
}

void TestQueuedActionCompletionCancellationAndDeadline() {
    using Result = UiDispatcher::ActionResult;
    using Clock = std::chrono::steady_clock;
    for (int scenario = 0; scenario != 3; ++scenario) {
        auto window = MakeWindow();
        std::promise<UiDispatcher::Task> queued;
        auto delivery = queued.get_future();
        UiDispatcher dispatcher(window.get(), [&](auto task) {
            queued.set_value(std::move(task));
            return true;
        });
        std::stop_source stop;
        int calls = 0;
        auto result = std::async(std::launch::async, [&] {
            auto deadline = scenario == 2 ? Clock::now() + std::chrono::milliseconds(40) : Clock::time_point::max();
            return dispatcher.RunAndWait(
                [&] {
                    ++calls;
                    return true;
                },
                stop.get_token(),
                deadline);
        });
        auto task = delivery.get();
        if (scenario == 0) task();
        if (scenario == 1) stop.request_stop();
        Check(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
              "completion, cancellation and deadline must each release the waiter");
        Check(result.get() == (scenario == 0 ? Result::Succeeded : Result::Failed),
              "queued action result must preserve admission");
        if (scenario != 0) task();
        Check(calls == (scenario == 0 ? 1 : 0), "late cancelled or expired deliveries must not execute");
    }
}

void TestCancellationOrShutdownDuringActionIsIndeterminate() {
    for (bool shutdown : std::array{false, true}) {
        auto window = MakeWindow();
        std::promise<UiDispatcher::Task> queued;
        auto delivery = queued.get_future();
        UiDispatcher dispatcher(window.get(), [&](auto task) {
            queued.set_value(std::move(task));
            return true;
        });
        std::stop_source stop;
        std::binary_semaphore entered(0), release(0);
        auto result = std::async(std::launch::async, [&] {
            return dispatcher.RunAndWait(
                [&] {
                    if (shutdown) dispatcher.Stop();
                    entered.release();
                    Check(release.try_acquire_for(std::chrono::seconds(2)),
                          "cancellation must not wait for running UI work");
                    return true;
                },
                stop.get_token(),
                std::chrono::steady_clock::time_point::max());
        });
        auto task = delivery.get();
        auto actual = UiDispatcher::ActionResult::Failed;
        std::jthread cancel([&] {
            entered.acquire();
            if (!shutdown) stop.request_stop();
            actual = result.get();
            release.release();
        });
        task();
        cancel.join();
        Check(actual == UiDispatcher::ActionResult::Indeterminate,
              "cancellation or shutdown after admission must not claim no UI effect");
    }
}

void TestShutdownWakesEveryActionWaiter() {
    auto window = MakeWindow();
    std::array<std::promise<UiDispatcher::Task>, 2> queued;
    std::atomic<std::size_t> index = 0;
    UiDispatcher dispatcher(window.get(), [&](auto task) {
        queued.at(index.fetch_add(1)).set_value(std::move(task));
        return true;
    });
    int calls = 0;
    auto invoke = [&] {
        return dispatcher.RunAndWait(
            [&] {
                ++calls;
                return true;
            },
            {},
            std::chrono::steady_clock::time_point::max());
    };
    auto first = std::async(std::launch::async, invoke);
    auto second = std::async(std::launch::async, invoke);
    auto firstTask = queued[0].get_future().get();
    auto secondTask = queued[1].get_future().get();
    dispatcher.Stop();
    Check(first.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              second.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "one persistent stop signal must release every waiter");
    Check(first.get() == UiDispatcher::ActionResult::Failed && second.get() == UiDispatcher::ActionResult::Failed,
          "shutdown must reject actions that never began");
    firstTask();
    secondTask();
    Check(calls == 0, "actions delivered after shutdown must not run");
}

} // namespace

int RunUiDispatcherTests() {
    TestInlineAndPrimaryDispatch();
    TestConcurrentNativeFallbackAndExceptions();
    TestStopReleasesCallbacksOutsideQueueLock();
    TestStopDrainsNativePostsBeforeWindowDestruction();
    TestLatePrimaryCallbackDoesNotRetainOwner();
    TestNativePostFailureIsRejected();
    TestActionInlineAdmissionAndResults();
    TestQueuedActionCompletionCancellationAndDeadline();
    TestCancellationOrShutdownDuringActionIsIndeterminate();
    TestShutdownWakesEveryActionWaiter();
    return g_failures;
}
