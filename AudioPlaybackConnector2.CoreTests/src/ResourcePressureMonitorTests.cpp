#include <app/ResourcePressureMonitor.hpp>
#include <app/ResourcePressureState.hpp>

#include <windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
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
    Check(low.IsBackgroundConstrained(), "acute low-memory state must suppress all optional background work");

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

void TestFailedProbesRevokePositiveAuthorization() {
    ResourcePressureStateReducer reducer;
    auto authorized = reducer.Apply({
        .LowMemorySignaled = false,
        .HighMemorySignaled = true,
        .UserActivity = UserActivityState::Available,
        .EnergySaver = false,
    });
    Check(authorized.CanPreload(), "fresh positive probes must authorize preloading");

    auto stale = reducer.Apply({
        .MemoryProbeAttempted = true,
        .UserActivityProbeAttempted = true,
        .EnergySaverProbeAttempted = true,
    });
    Check(stale.Memory == MemoryPressureState::Unknown && stale.UserActivity == UserActivityState::Unknown &&
              !stale.EnergySaver.has_value(),
          "failed probes must revoke stale positive memory, activity, and power authorization");
    Check(!stale.CanPreload() && stale.IsBackgroundConstrained(),
          "stale positive signals must never complete a speculative preload window");

    auto low = reducer.Apply({.LowMemorySignaled = true, .MemoryProbeAttempted = true});
    auto failedAfterLow = reducer.Apply({.MemoryProbeAttempted = true});
    Check(low.IsMemoryPressure() && failedAfterLow.IsMemoryPressure(),
          "failed memory probes must conservatively preserve a known acute low-memory state");
}

void TestPartialMemoryProbeFailuresRemainFailClosed() {
    ResourcePressureStateReducer reducer;
    auto missingLow = reducer.Apply({.HighMemorySignaled = true, .MemoryProbeAttempted = true});
    Check(missingLow.Memory == MemoryPressureState::Unknown && !missingLow.CanPreload(),
          "a high-memory signal cannot authorize preloading when the low-memory probe failed");

    auto high = reducer.Apply({.LowMemorySignaled = false, .HighMemorySignaled = true});
    Check(high.Memory == MemoryPressureState::High, "two complete memory probes may authorize a high state");
    auto missingHigh = reducer.Apply({.LowMemorySignaled = false, .MemoryProbeAttempted = true});
    Check(missingHigh.Memory == MemoryPressureState::Neutral && !missingHigh.CanPreload(),
          "a cleared low-memory probe must revoke high authorization when the high probe fails");

    auto low = reducer.Apply({.LowMemorySignaled = true, .MemoryProbeAttempted = true});
    Check(low.Memory == MemoryPressureState::Low,
          "an acute low-memory signal must remain actionable even when the high-memory probe fails");
    auto incompleteRecovery = reducer.Apply({.LowMemorySignaled = false, .MemoryProbeAttempted = true});
    Check(incompleteRecovery.Memory == MemoryPressureState::Neutral && !incompleteRecovery.CanPreload(),
          "a cleared low-memory object may end acute pressure without inventing high capacity");
    auto completeRecovery = reducer.Apply({.LowMemorySignaled = false, .HighMemorySignaled = false});
    Check(completeRecovery.Memory == MemoryPressureState::Neutral,
          "two complete negative probes may clear an acute low-memory state");
}

void TestSnapshotFreshnessFailsClosed() {
    using Clock = std::chrono::steady_clock;
    auto const now = Clock::time_point{} + 100s;
    Check(IsResourcePressureSnapshotFresh(now - 74s, now, 75s),
          "a recent resource snapshot must remain usable inside its freshness window");
    Check(!IsResourcePressureSnapshotFresh(now - 75s, now, 75s),
          "a resource snapshot must expire exactly at its maximum age");
    Check(!IsResourcePressureSnapshotFresh(std::nullopt, now, 75s),
          "a missing resource snapshot must never authorize optional work");
    Check(!IsResourcePressureSnapshotFresh(now + 1s, now, 75s),
          "a future resource timestamp must fail closed after a clock anomaly");
    Check(!IsResourcePressureSnapshotFresh(now, now, 0ms),
          "a non-positive freshness window must disable positive authorization");
}

void TestIncompleteMemoryProbesPauseSignalWaits() {
    auto incomplete = PlanMemoryNotificationWaits(std::nullopt, false);
    Check(!incomplete.ArmLow && !incomplete.ArmHigh,
          "incomplete memory probes must pause both waits until the periodic poll retries them");

    auto neutral = PlanMemoryNotificationWaits(false, false);
    Check(neutral.ArmLow && neutral.ArmHigh, "complete neutral probes must monitor both pressure transitions");
    auto low = PlanMemoryNotificationWaits(true, false);
    Check(!low.ArmLow && low.ArmHigh, "a signaled low wait must stay paused while high recovery remains monitored");
    auto high = PlanMemoryNotificationWaits(false, true);
    Check(high.ArmLow && !high.ArmHigh, "a signaled high wait must stay paused while low pressure remains monitored");
    auto conflicting = PlanMemoryNotificationWaits(true, true);
    Check(!conflicting.ArmLow && !conflicting.ArmHigh,
          "contradictory signaled notifications must pause both waits instead of creating a callback storm");
}

void TestPositiveAuthorizationAndAdaptivePollCadence() {
    Check(IsPositiveResourceAuthorizationCurrent(11, 11),
          "the UI snapshot that observed the latest constraint may make a new decision");
    Check(!IsPositiveResourceAuthorizationCurrent(10, 11),
          "an older UI snapshot must not override a newer constrained worker snapshot");

    ResourcePressureValues desktop{
        .Memory = MemoryPressureState::High,
        .UserActivity = UserActivityState::Available,
        .EnergySaver = false,
    };
    Check(!ShouldUseConstrainedPollInterval(desktop, true),
          "active desktop monitoring must retain the responsive poll cadence");
    desktop.UserActivity = UserActivityState::Fullscreen;
    Check(ShouldUseConstrainedPollInterval(desktop, true),
          "stable fullscreen activity must use the low-wakeup constrained cadence");
    Check(!ShouldUseConstrainedPollInterval(desktop, false),
          "incomplete probes must retry promptly instead of hiding uncertainty behind a slow cadence");
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
    std::atomic_bool stopOnNextCallback = true;
    ResourcePressureMonitor* monitorAddress = nullptr;

    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            if (!stopOnNextCallback.exchange(false)) return;
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

    static_cast<void>(monitor.Start());
    Check(WaitFor(changed, mutex, 2s, [&] { return callbackReturned; }),
          "Stop from a public callback must not deadlock");
    Check(!monitor.IsRunning(), "self-stop must leave the monitor stopped");
    Check(callbackRestartRejected, "restart from an active callback must be rejected instead of overlapping contexts");
    Check(monitor.Start(), "restart after a self-stopping callback must wait for retirement and then succeed");
    monitor.Stop();
}

void TestMonitorCanBeDestroyedFromItsOwnCallback() {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool startReturned = false;
    bool destroyed = false;
    std::unique_ptr<ResourcePressureMonitor> monitor;
    monitor = std::make_unique<ResourcePressureMonitor>(
        [&](ResourcePressureSnapshot const&) {
            while (!startReturned.load()) {
                std::this_thread::yield();
            }
            monitor.reset();
            {
                std::scoped_lock lock(mutex);
                destroyed = true;
            }
            changed.notify_all();
        },
        ResourcePressureMonitor::Config{.PollInterval = 25ms});

    auto* monitorAddress = monitor.get();
    Check(monitorAddress->Start(), "self-destroying monitor must start");
    startReturned.store(true);
    Check(WaitFor(changed, mutex, 2s, [&] { return destroyed; }),
          "destruction from a public callback must not deadlock");
    Check(!monitor, "self-destruction must release the monitor instance");
}

void TestExternalStopWaitsForSelfStoppedCallback() {
    std::mutex mutex;
    std::condition_variable changed;
    bool callbackEntered = false;
    bool releaseCallback = false;
    std::atomic_bool externalStopEntered = false;
    std::atomic_bool externalStopReturned = false;
    ResourcePressureMonitor* monitorAddress = nullptr;
    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            monitorAddress->Stop();
            std::unique_lock lock(mutex);
            callbackEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return releaseCallback; });
        },
        {.PollInterval = 25ms});
    monitorAddress = &monitor;

    static_cast<void>(monitor.Start());
    Check(WaitFor(changed, mutex, 2s, [&] { return callbackEntered; }),
          "self-stopping callback must enter its controlled wait");

    std::thread externalStop([&] {
        externalStopEntered.store(true);
        changed.notify_all();
        monitor.Stop();
        externalStopReturned.store(true);
        changed.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, 2s, [&] { return externalStopEntered.load(); }),
              "external Stop thread must enter before its barrier is evaluated");
        static_cast<void>(changed.wait_for(lock, 100ms, [&] { return externalStopReturned.load(); }));
        Check(!externalStopReturned.load(), "an external Stop must remain a barrier for a self-stopped callback");
        releaseCallback = true;
    }
    changed.notify_all();
    externalStop.join();
    Check(externalStopReturned.load(), "external Stop must finish after the public callback returns");
}

void TestMonitorPublishesPeriodicLivenessHeartbeat() {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t callbacks = 0;
    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            {
                std::scoped_lock lock(mutex);
                ++callbacks;
            }
            changed.notify_all();
        },
        {.PollInterval = 25ms, .ConstrainedPollInterval = 25ms, .SnapshotHeartbeatInterval = 75ms});

    Check(monitor.Start(), "heartbeat monitor must start");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks >= 2; }),
          "unchanged sensor values must still publish a bounded liveness heartbeat");
    monitor.Stop();
}

void TestExplicitProbeShortensLongPoll() {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t callbacks = 0;
    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            {
                std::scoped_lock lock(mutex);
                ++callbacks;
            }
            changed.notify_all();
        },
        {.PollInterval = 30s, .ConstrainedPollInterval = 30s, .SnapshotHeartbeatInterval = 1ms});

    Check(!monitor.RequestProbe(), "an explicit probe must be rejected before the monitor starts");
    Check(monitor.Start(), "explicit-probe monitor must start");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks >= 1; }),
          "explicit-probe monitor must publish its initial snapshot");
    std::this_thread::sleep_for(10ms);
    Check(monitor.RequestProbe(), "a running monitor must accept an explicit probe request");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks >= 2; }),
          "an explicit probe must not wait for the long periodic interval");
    monitor.Stop();
    Check(!monitor.RequestProbe(), "an explicit probe must be rejected after the monitor stops");
}

void TestExplicitProbeFromCallbackRemainsSafe() {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t callbacks = 0;
    std::atomic_bool requestAccepted = false;
    ResourcePressureMonitor* monitorAddress = nullptr;
    ResourcePressureMonitor monitor(
        [&](ResourcePressureSnapshot const&) {
            std::size_t callbackNumber = 0;
            {
                std::scoped_lock lock(mutex);
                callbackNumber = ++callbacks;
            }
            if (callbackNumber == 1) {
                requestAccepted.store(monitorAddress->RequestProbe());
            }
            changed.notify_all();
        },
        {.PollInterval = 30s, .ConstrainedPollInterval = 30s, .SnapshotHeartbeatInterval = 1ms});
    monitorAddress = &monitor;

    Check(monitor.Start(), "callback-probe monitor must start");
    Check(WaitFor(changed, mutex, 2s, [&] { return callbacks >= 2; }),
          "an explicit probe requested from a callback must not deadlock");
    Check(requestAccepted.load(), "the active callback must be able to request a follow-up probe");
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

void TestRepeatedLifecycleDoesNotLeakHandles() {
    ResourcePressureMonitor monitor([](ResourcePressureSnapshot const&) {}, {.PollInterval = 25ms});
    Check(monitor.Start(), "handle lifecycle warmup must start");
    monitor.Stop();

    DWORD handlesBefore = 0;
    DWORD handlesAfter = 0;
    Check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE,
          "test process handle baseline must be observable");
    for (std::size_t iteration = 0; iteration < 100; ++iteration) {
        Check(monitor.Start(), "repeated resource monitor start must succeed");
        Check(monitor.RequestProbe(), "a running lifecycle iteration must accept an explicit probe");
        monitor.Stop();
    }
    Check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE,
          "test process handle result must be observable");
    Check(handlesAfter <= handlesBefore + 2,
          "repeated resource monitor start/stop must not retain notification, timer, or wait handles");
}
} // namespace

int RunResourcePressureMonitorTests() {
    TestReducerHandlesMemoryTransitionsAndPartialFailures();
    TestReducerPreservesIndependentSignals();
    TestFailedProbesRevokePositiveAuthorization();
    TestPartialMemoryProbeFailuresRemainFailClosed();
    TestSnapshotFreshnessFailsClosed();
    TestIncompleteMemoryProbesPauseSignalWaits();
    TestPositiveAuthorizationAndAdaptivePollCadence();
    TestMonitorLifecycleAndLateCallbackBarrier();
    TestMonitorCanStopFromItsOwnCallback();
    TestMonitorCanBeDestroyedFromItsOwnCallback();
    TestExternalStopWaitsForSelfStoppedCallback();
    TestMonitorPublishesPeriodicLivenessHeartbeat();
    TestExplicitProbeShortensLongPoll();
    TestExplicitProbeFromCallbackRemainsSafe();
    TestConcurrentStartAndStopRemainSafe();
    TestRepeatedLifecycleDoesNotLeakHandles();
    return g_failures;
}
