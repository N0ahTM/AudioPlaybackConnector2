#include <app/ResourcePressureMonitor.hpp>
#include <app/ResourcePressureState.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestReducerHandlesMemoryTransitionsAndPartialFailures() {
    ResourcePressureStateReducer reducer;
    Check(reducer.Current().Memory == MemoryPressureState::Unknown, "memory state must start unknown");
    Check(!reducer.Current().IsMemoryPressure(), "unknown memory state must not trigger an acute low-memory release");
    Check(!reducer.Current().HasReliableMemoryState() && reducer.Current().IsBackgroundConstrained(),
          "unknown memory state must prevent speculative preloading until a reliable probe succeeds");
    Check(!reducer.Current().CanPreload(), "unknown memory state must never authorize preloading");

    auto low = reducer.Apply({.LowMemorySignaled = true, .HighMemorySignaled = false});
    Check(low.Memory == MemoryPressureState::Low && low.IsMemoryPressure(),
          "a signaled low-memory object must take priority");

    auto unchanged = reducer.Apply({});
    Check(unchanged.Memory == MemoryPressureState::Low, "failed probes must preserve the last reliable memory state");

    auto neutral = reducer.Apply({.LowMemorySignaled = false});
    Check(neutral.Memory == MemoryPressureState::Neutral,
          "a cleared low-memory object must not leave stale memory pressure behind");
    Check(neutral.HasReliableMemoryState(), "a successfully cleared pressure signal must restore memory reliability");
    Check(!neutral.CanPreload(), "neutral memory range must keep usage stable instead of authorizing preloading");

    auto high = reducer.Apply({.LowMemorySignaled = false, .HighMemorySignaled = true});
    Check(high.Memory == MemoryPressureState::High, "a signaled high-memory object must report available capacity");

    auto clearedHigh = reducer.Apply({.HighMemorySignaled = false});
    Check(clearedHigh.Memory == MemoryPressureState::Neutral,
          "a cleared high-memory object must return to the neutral range");

    auto conflicting = reducer.Apply({.LowMemorySignaled = true, .HighMemorySignaled = true});
    Check(conflicting.Memory == MemoryPressureState::Low,
          "contradictory memory signals must choose the conservative low-memory state");
}

void TestReducerPreservesIndependentSignals() {
    ResourcePressureStateReducer reducer;
    Check(reducer.Current().IsBackgroundConstrained(),
          "unknown activity and energy-saver state must conservatively suppress preloading");
    auto desktopWithoutMemory = reducer.Apply({.UserActivity = UserActivityState::Available, .EnergySaver = false});
    Check(desktopWithoutMemory.Memory == MemoryPressureState::Unknown &&
              desktopWithoutMemory.IsBackgroundConstrained() && !desktopWithoutMemory.IsMemoryPressure(),
          "available desktop signals must not permit preloading while every memory probe is unknown");
    auto fullscreen = reducer.Apply({.UserActivity = UserActivityState::Fullscreen, .EnergySaver = true});
    Check(fullscreen.IsBackgroundConstrained(), "fullscreen and energy saver must constrain background work");

    auto preserved = reducer.Apply({.LowMemorySignaled = false, .HighMemorySignaled = false});
    Check(preserved.UserActivity == UserActivityState::Fullscreen && preserved.EnergySaver == true,
          "unavailable periodic probes must preserve their independent prior values");

    auto available = reducer.Apply({.UserActivity = UserActivityState::Available, .EnergySaver = false});
    Check(!available.IsBackgroundConstrained(), "available desktop state must remove activity and power constraints");
    auto preloadable = reducer.Apply({.LowMemorySignaled = false, .HighMemorySignaled = true});
    Check(preloadable.CanPreload(), "only a high-memory, available desktop without energy saver may preload");

    auto quiet = reducer.Apply({.UserActivity = UserActivityState::QuietTime});
    Check(!quiet.IsBackgroundConstrained(), "Windows quiet-time alone must not be mistaken for fullscreen pressure");

    auto notPresent = reducer.Apply({.UserActivity = UserActivityState::NotPresent});
    Check(notPresent.IsBackgroundConstrained(), "a locked or absent user session must suppress background preloading");

    auto app = reducer.Apply({.UserActivity = UserActivityState::ImmersiveApp});
    Check(app.IsBackgroundConstrained(), "an immersive app must constrain background preloading");
}

bool WaitFor(std::condition_variable& changed, std::mutex& mutex, std::chrono::milliseconds timeout, auto&& predicate) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, timeout, std::forward<decltype(predicate)>(predicate));
}

void TestMonitorLifecycleAndLateCallbackBarrier() {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t callbacks = 0;
    std::uint64_t lastSequence = 0;
    bool ordered = true;

    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const& snapshot) {
            std::scoped_lock lock(mutex);
            ordered = ordered && snapshot.Sequence > lastSequence;
            lastSequence = snapshot.Sequence;
            ++callbacks;
            changed.notify_all();
        },
        {.PollInterval = 25ms});

    Check(monitor.Start(), "resource pressure monitor must start with Windows threadpool resources");
    Check(monitor.Start(), "starting an active monitor must be idempotent");
    Check(monitor.IsRunning(), "a started monitor must report running");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks != 0; }),
          "the initial asynchronous resource snapshot must be delivered");

    monitor.Stop();
    monitor.Stop();
    Check(!monitor.IsRunning(), "stopping a monitor must be idempotent");
    std::size_t callbacksAtStop = 0;
    {
        std::scoped_lock lock(mutex);
        callbacksAtStop = callbacks;
    }
    std::this_thread::sleep_for(100ms);
    {
        std::scoped_lock lock(mutex);
        Check(callbacks == callbacksAtStop, "Stop must form a barrier against late public callbacks");
        Check(ordered, "resource snapshots must be delivered in strictly increasing sequence order");
    }

    Check(monitor.Start(), "a stopped monitor must support a clean restart");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks > callbacksAtStop; }),
          "a restarted monitor must publish a new initial snapshot");
    monitor.Stop();
    {
        std::scoped_lock lock(mutex);
        Check(ordered, "snapshot sequence numbers must remain monotonic across monitor restarts");
    }
}

void TestMonitorCanStopFromItsOwnCallback() {
    std::mutex mutex;
    std::condition_variable changed;
    bool callbackReturned = false;
    bool callbackRestartRejected = false;
    ResourcePressureMonitor* monitorAddress = nullptr;

    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            monitorAddress->Stop();
            callbackRestartRejected = !monitorAddress->Start();
            {
                std::scoped_lock lock(mutex);
                callbackReturned = true;
            }
            changed.notify_all();
        },
        {.PollInterval = 25ms});
    monitorAddress = &monitor;

    Check(monitor.Start(), "self-stopping monitor must start");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbackReturned; }),
          "Stop from a public callback must not deadlock");
    Check(!monitor.IsRunning(), "self-stop must leave the monitor stopped");
    Check(callbackRestartRejected, "restart from an active callback must be rejected instead of overlapping contexts");
    Check(monitor.Start(), "restart after a self-stopping callback must wait for retirement and then succeed");
    monitor.Stop();
}

void TestConcurrentStartAndStopRemainSafe() {
    std::atomic_bool startFailed = false;
    ResourcePressureMonitor monitor([](ResourcePressureSnapshot const&) {}, {.PollInterval = -1ms});

    std::array<std::thread, 4> workers;
    for (std::size_t worker = 0; worker < workers.size(); ++worker) {
        workers[worker] = std::thread([&, worker] {
            for (std::size_t iteration = 0; iteration < 25; ++iteration) {
                if ((worker + iteration) % 2 == 0) {
                    if (!monitor.Start()) startFailed.store(true);
                } else {
                    monitor.Stop();
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    monitor.Stop();

    Check(!startFailed.load(), "concurrent starts must not lose threadpool resource initialization");
    Check(!monitor.IsRunning(), "a final stop must win after concurrent lifecycle operations");
}
} // namespace

int RunResourcePressureMonitorTests() {
    TestReducerHandlesMemoryTransitionsAndPartialFailures();
    TestReducerPreservesIndependentSignals();
    TestMonitorLifecycleAndLateCallbackBarrier();
    TestMonitorCanStopFromItsOwnCallback();
    TestConcurrentStartAndStopRemainSafe();
    return g_failures;
}
