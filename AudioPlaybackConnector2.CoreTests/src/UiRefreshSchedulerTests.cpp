#include "TestCheck.hpp"
#include <app/UiRefreshScheduler.hpp>
#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Test Dispatcher ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct UiQueue {
    std::mutex Mutex;
    std::condition_variable Changed;
    std::deque<UiRefreshScheduler::Task> Tasks;
    unsigned int Reject = 0;

    bool Enqueue(UiRefreshScheduler::Task task) {
        std::lock_guard lock(Mutex);
        if (Reject) {
            --Reject;
            return false;
        }
        Tasks.push_back(std::move(task));
        Changed.notify_all();
        return true;
    }
    UiRefreshScheduler::Task Take() {
        std::unique_lock lock(Mutex);
        if (!Changed.wait_for(lock, 5s, [&] { return !Tasks.empty(); })) {
            Check(false, "native scheduler must deliver within the test watchdog");
            return [] {};
        }
        auto task = std::move(Tasks.front());
        Tasks.pop_front();
        return task;
    }
    bool Empty() {
        std::lock_guard lock(Mutex);
        return Tasks.empty();
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Scheduler Tests ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestCoalescingAndStaleDelivery() {
    auto queue = std::make_shared<UiQueue>();
    std::vector<unsigned int> rendered;
    UiRefreshScheduler* current = nullptr;
    UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                 [&](auto flags) {
                                     rendered.push_back(flags);
                                     if (rendered.size() == 1) current->Request(4);
                                     return true;
                                 },
                                 ~0U);
    current = &scheduler;
    scheduler.Request(1);
    scheduler.Request(2);
    auto first = queue->Take();
    auto duplicate = first;
    first();
    duplicate();
    Check(rendered == std::vector<unsigned int>{3}, "duplicate delivery must not consume the next generation");
    queue->Take()();
    Check(rendered == std::vector<unsigned int>({3, 4}) && queue->Empty(),
          "requests during rendering must produce exactly one additional UI pass");
}

void TestNativeRetryRetainsRequestsAndAppliesRetryMask() {
    auto queue = std::make_shared<UiQueue>();
    queue->Reject = 1;
    std::vector<unsigned int> rendered;
    UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                 [&](auto flags) {
                                     rendered.push_back(flags);
                                     return rendered.size() > 1;
                                 },
                                 ~2U);
    scheduler.Request(1);
    scheduler.Request(2);
    queue->Take()();
    queue->Take()();
    Check(rendered == std::vector<unsigned int>({3, 1}) && queue->Empty(),
          "native dispatch retry must retain flags; render retry must omit one-shot flags");
}

void TestRenderingExceptionCanRecover() {
    auto queue = std::make_shared<UiQueue>();
    unsigned int calls = 0;
    UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                 [&](auto) {
                                     if (++calls == 1) throw std::runtime_error("render");
                                     return true;
                                 },
                                 ~0U);
    scheduler.Request(1);
    queue->Take()();
    queue->Take()();
    Check(calls == 2 && queue->Empty(), "a rendering exception must preserve work for a bounded retry");
}

void TestConcurrentRequestsAndTerminalStop() {
    auto queue = std::make_shared<UiQueue>();
    unsigned int rendered = 0;
    UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                 [&](auto flags) {
                                     rendered = flags;
                                     return true;
                                 },
                                 ~0U);
    std::barrier start{16};
    std::vector<std::jthread> producers;
    for (unsigned int bit = 0; bit < 16; ++bit)
        producers.emplace_back([&, bit] {
            start.arrive_and_wait();
            scheduler.Request(1U << bit);
        });
    producers.clear();
    queue->Take()();
    Check(rendered == 0xffff && queue->Empty(), "concurrent producers must merge into one complete UI pass");
    scheduler.Request(1);
    auto stale = queue->Take();
    scheduler.Stop();
    stale();
    scheduler.Request(2);
    Check(rendered == 0xffff && queue->Empty(), "Stop must invalidate queued work and reject new requests");
}

void TestQueuedDeliveryOutlivesFacade() {
    auto queue = std::make_shared<UiQueue>();
    bool rendered = false;
    {
        UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                     [&](auto) {
                                         rendered = true;
                                         return true;
                                     },
                                     ~0U);
        scheduler.Request(1);
    }
    queue->Take()();
    Check(!rendered, "queued weak delivery must be harmless after scheduler destruction");
}

void TestStopDoesNotWaitForAdmittedRendering() {
    auto queue = std::make_shared<UiQueue>();
    std::binary_semaphore entered{0}, release{0};
    UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                 [&](auto) {
                                     entered.release();
                                     release.acquire();
                                     return false;
                                 },
                                 ~0U);
    scheduler.Request(1);
    std::jthread ui([task = queue->Take()] { task(); });
    const bool running = entered.try_acquire_for(5s);
    Check(running, "rendering must enter before concurrent Stop");
    scheduler.Stop();
    release.release();
    ui.join();
    Check(queue->Empty(), "late render failure must not rearm a stopped scheduler");
}

void TestReentrantStopAndDispatchException() {
    auto queue = std::make_shared<UiQueue>();
    std::atomic_bool reject{true};
    unsigned int rendered = 0;
    UiRefreshScheduler* current = nullptr;
    UiRefreshScheduler scheduler(
        [queue, &reject](auto task) {
            if (reject.exchange(false)) throw std::runtime_error("dispatch");
            return queue->Enqueue(std::move(task));
        },
        [&](auto) {
            ++rendered;
            current->Stop();
            current->Request(2);
            return false;
        },
        ~0U);
    current = &scheduler;
    scheduler.Request(1);
    queue->Take()();
    Check(rendered == 1 && queue->Empty(), "dispatch exceptions recover; reentrant Stop prevents a render retry");
}

void TestRequestRacesStop() {
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto queue = std::make_shared<UiQueue>();
        bool rendered = false;
        UiRefreshScheduler scheduler([queue](auto task) { return queue->Enqueue(std::move(task)); },
                                     [&](auto) {
                                         rendered = true;
                                         return true;
                                     },
                                     ~0U);
        std::barrier race{2};
        std::jthread requester([&] {
            race.arrive_and_wait();
            scheduler.Request(1);
        });
        race.arrive_and_wait();
        scheduler.Stop();
        requester.join();
        while (!queue->Empty())
            queue->Take()();
        Check(!rendered, "a request racing Stop must not render after terminal admission closure");
    }
}

void TestDestructionDuringNativeDispatch() {
    struct Probe {
        std::binary_semaphore Entered{0}, Release{0}, Finished{0};
        std::atomic_uint Calls{0};
    };
    auto probe = std::make_shared<Probe>();
    auto scheduler = std::make_unique<UiRefreshScheduler>(
        [probe](auto) {
            if (probe->Calls.fetch_add(1) != 0) {
                probe->Entered.release();
                probe->Release.acquire();
                probe->Finished.release();
            }
            return false;
        },
        [](auto) { return true; },
        ~0U);
    scheduler->Request(1);
    const bool entered = probe->Entered.try_acquire_for(5s);
    Check(entered, "native retry must enter the blocked dispatch probe");
    const auto before = std::chrono::steady_clock::now();
    scheduler.reset();
    Check(std::chrono::steady_clock::now() - before < 1s,
          "destruction must not wait for disassociated native delivery");
    probe->Release.release();
    if (entered)
        Check(probe->Finished.try_acquire_for(5s), "admitted native dispatch must finish after owner destruction");
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Test Entry Point //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunUiRefreshSchedulerTests() {
    TestCoalescingAndStaleDelivery();
    TestNativeRetryRetainsRequestsAndAppliesRetryMask();
    TestRenderingExceptionCanRecover();
    TestConcurrentRequestsAndTerminalStop();
    TestQueuedDeliveryOutlivesFacade();
    TestStopDoesNotWaitForAdmittedRendering();
    TestDestructionDuringNativeDispatch();
    TestReentrantStopAndDispatchException();
    TestRequestRacesStop();
    return g_failures;
}
