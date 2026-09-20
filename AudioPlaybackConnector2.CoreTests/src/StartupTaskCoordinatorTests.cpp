#include "TestCheck.hpp"
#include "AppTestFixture.hpp"

#include <app/StartupTaskCoordinator.hpp>

#include <windows.h>
#include <winrt/Windows.ApplicationModel.h>
#include <wil/resource.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <future>
#include <semaphore>

namespace {
using namespace std::chrono_literals;

class Event {
public:
    Event() : m_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!m_handle) throw std::runtime_error("CreateEventW failed");
    }

    Event(Event const&) = delete;
    Event& operator=(Event const&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return m_handle.get(); }
    void Signal() const noexcept { SetEvent(m_handle.get()); }

private:
    wil::unique_handle m_handle;
};

class FakeStartupTaskBackend {
public:
    enum class CallKind { Query, Set };

    struct Gate {
        explicit Gate(CallKind kind, bool desired = false) : Kind(kind), Desired(desired) {}

        Event Completed;
        CallKind Kind;
        bool Desired = false;
        std::atomic_bool Result = false;
        std::atomic_bool Throw = false;
    };

    winrt::Windows::Foundation::IAsyncOperation<bool> QueryAsync() {
        auto gate = AddGate(CallKind::Query);
        co_await winrt::resume_on_signal(gate->Completed.Get());
        if (gate->Throw.load()) throw std::runtime_error("query failed");
        co_return gate->Result.load();
    }

    winrt::Windows::Foundation::IAsyncOperation<bool> SetAsync(bool desired) {
        auto gate = AddGate(CallKind::Set, desired);
        co_await winrt::resume_on_signal(gate->Completed.Get());
        if (gate->Throw.load()) throw std::runtime_error("set failed");
        co_return gate->Result.load();
    }

    [[nodiscard]] bool WaitForCalls(std::size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&]() { return m_gates.size() >= count; });
    }

    [[nodiscard]] std::shared_ptr<Gate> GateAt(std::size_t index) const {
        std::scoped_lock lock(m_mutex);
        return m_gates.at(index);
    }

    [[nodiscard]] std::size_t CallCount() const {
        std::scoped_lock lock(m_mutex);
        return m_gates.size();
    }

    void Release(std::size_t index, bool result, bool shouldThrow = false) {
        auto gate = GateAt(index);
        gate->Result.store(result);
        gate->Throw.store(shouldThrow);
        gate->Completed.Signal();
    }

private:
    [[nodiscard]] std::shared_ptr<Gate> AddGate(CallKind kind, bool desired = false) {
        auto gate = std::make_shared<Gate>(kind, desired);
        {
            std::scoped_lock lock(m_mutex);
            m_gates.push_back(gate);
        }
        m_changed.notify_all();
        return gate;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::shared_ptr<Gate>> m_gates;
};

class SnapshotRecorder {
public:
    void Record(StartupTaskSnapshot const& snapshot) {
        {
            std::scoped_lock lock(m_mutex);
            m_snapshots.push_back(snapshot);
        }
        m_changed.notify_all();
    }

    [[nodiscard]] bool WaitFor(std::function<bool(StartupTaskSnapshot const&)> predicate,
                               std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout, [&]() { return !m_snapshots.empty() && predicate(m_snapshots.back()); });
    }

    [[nodiscard]] StartupTaskSnapshot Latest() const {
        std::scoped_lock lock(m_mutex);
        return m_snapshots.empty() ? StartupTaskSnapshot{} : m_snapshots.back();
    }

    [[nodiscard]] std::size_t Count() const {
        std::scoped_lock lock(m_mutex);
        return m_snapshots.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<StartupTaskSnapshot> m_snapshots;
};

struct CoordinatorFixture {
    CoordinatorFixture() : Backend(std::make_shared<FakeStartupTaskBackend>()) {
        Coordinator = std::make_shared<StartupTaskCoordinator>(
            [backend = Backend]() { return backend->QueryAsync(); },
            [backend = Backend](bool desired) { return backend->SetAsync(desired); },
            [this](bool actual) {
                {
                    std::scoped_lock lock(CommitMutex);
                    Commits.push_back(actual);
                }
                CommitChanged.notify_all();
            });
    }

    ~CoordinatorFixture() { Coordinator->Shutdown(); }

    [[nodiscard]] bool WaitForCommits(std::size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(CommitMutex);
        return CommitChanged.wait_for(lock, timeout, [&]() { return Commits.size() >= count; });
    }

    [[nodiscard]] std::vector<bool> CommitSnapshot() const {
        std::scoped_lock lock(CommitMutex);
        return Commits;
    }

    std::shared_ptr<FakeStartupTaskBackend> Backend;
    std::shared_ptr<StartupTaskCoordinator> Coordinator;
    mutable std::mutex CommitMutex;
    std::condition_variable CommitChanged;
    std::vector<bool> Commits;
};

void TestCommitCanReenterAndStopTheOwner() {
    auto backend = std::make_shared<FakeStartupTaskBackend>();
    std::shared_ptr<StartupTaskCoordinator> owner;
    std::promise<void> committed;
    owner = std::make_shared<StartupTaskCoordinator>(
        [backend] { return backend->QueryAsync(); },
        [backend](bool desired) { return backend->SetAsync(desired); },
        [&](bool) {
            Check(owner->RequestDesired(false), "a commit callback must be able to enqueue a new intent");
            owner->Shutdown();
            committed.set_value();
        });
    owner->Refresh();
    Check(backend->WaitForCalls(1), "reentrant commit test must start its query");
    backend->Release(0, true);
    Check(committed.get_future().wait_for(2s) == std::future_status::ready,
          "commit callbacks must run outside owner locks and allow reentrant shutdown");
    Check(!owner->RequestDesired(true) && !owner->Refresh() && !owner->Snapshot().Busy,
          "shutdown from a commit must discard its queued replacement request and close admission");
    Check(!backend->WaitForCalls(2, 100ms), "a queued intent must not start after reentrant shutdown");
}

void TestShutdownDrainsForeignCommitWithoutHoldingOwnerLock() {
    auto backend = std::make_shared<FakeStartupTaskBackend>();
    std::binary_semaphore entered{0}, release{0};
    auto owner =
        std::make_shared<StartupTaskCoordinator>([backend] { return backend->QueryAsync(); },
                                                 [backend](bool desired) { return backend->SetAsync(desired); },
                                                 [&](bool) {
                                                     entered.release();
                                                     release.acquire();
                                                 });
    owner->Refresh();
    Check(backend->WaitForCalls(1), "drain test must start its query");
    backend->Release(0, true);
    Check(entered.try_acquire_for(2s), "the foreign completion must enter persistence");
    auto stopped = std::async(std::launch::async, [&] { owner->Shutdown(); });
    Check(stopped.wait_for(50ms) == std::future_status::timeout,
          "shutdown must drain an admitted foreign commit before returning");
    auto snapshot = std::async(std::launch::async, [&] { return owner->Snapshot(); });
    Check(snapshot.wait_for(2s) == std::future_status::ready,
          "an admitted commit and shutdown must not retain the owner's snapshot mutex");
    release.release();
    Check(stopped.wait_for(2s) == std::future_status::ready, "shutdown must finish when the admitted commit leaves");
}

void TestFacadeDestructionInvalidatesPendingSetContinuation() {
    auto backend = std::make_shared<FakeStartupTaskBackend>();
    auto owner =
        std::make_shared<StartupTaskCoordinator>([backend] { return backend->QueryAsync(); },
                                                 [backend](bool desired) { return backend->SetAsync(desired); },
                                                 [](bool) {});
    std::weak_ptr<StartupTaskCoordinator> weak = owner;
    owner->RequestDesired(true);
    Check(backend->WaitForCalls(1), "destruction test must suspend in its set operation");
    owner.reset();
    Check(weak.expired(), "an OS operation must not retain the coordinator facade");
    backend->Release(0, true);
    Check(!backend->WaitForCalls(2, 100ms), "a late set completion must not launch a query after shutdown");
}

void TestAppControllerOwnsStartupActionsAndObservation() {
    apc::tests::AppFixture app;
    CoordinatorFixture fixture;
    using Controller = apc::app::AppController;
    Controller controller(app.Settings, app.Service, app.Presentation, fixture.Coordinator);
    SnapshotRecorder recorder;
    auto observation = controller.SnapshotAndSubscribe([&](auto const& notification) {
        if (auto startup = std::get_if<apc::app::StartupTaskChangedEvent>(&notification.Event))
            recorder.Record(startup->Snapshot);
    });
    Check(observation.Updates && observation.Snapshot.StartupTask && !observation.Snapshot.StartupTask->Known,
          "the application observation must expose the coordinator without requiring a second UI subscription");
    Check(controller.RefreshStartupTask() == Controller::StartupTaskRequestResult::Accepted,
          "refresh must be admitted through the application endpoint");
    Check(fixture.Backend->WaitForCalls(1), "the application refresh must reach the backend");
    fixture.Backend->Release(0, false);
    Check(recorder.WaitFor([](auto const& state) { return state.Known && !state.Busy; }),
          "authoritative startup changes must flow through the ordered application stream");
    auto confirmed = controller.Snapshot();
    Check(confirmed.StartupTask == fixture.Coordinator->Snapshot() &&
              confirmed.Generation > observation.Snapshot.Generation,
          "the application snapshot must expose the current owner's publication and advance presentation generation");
    Check(controller.SetStartWithWindows(true) == Controller::StartupTaskRequestResult::Accepted,
          "the startup toggle must use the same application endpoint");
    Check(fixture.Backend->WaitForCalls(2), "the toggle must start one set operation");
    controller.Shutdown();
    auto const delivered = recorder.Count();
    Check(controller.SetStartWithWindows(false) == Controller::StartupTaskRequestResult::Unavailable &&
              controller.RefreshStartupTask() == Controller::StartupTaskRequestResult::Unavailable,
          "controller shutdown must close both startup actions");
    fixture.Backend->Release(1, true);
    Check(fixture.Backend->WaitForCalls(3), "the owner may settle its already admitted operation");
    fixture.Backend->Release(2, true);
    Check(fixture.WaitForCommits(2) && recorder.Count() == delivered,
          "an owner completion after application shutdown must not reach application observers");
    Check(app.Controller.RefreshStartupTask() == Controller::StartupTaskRequestResult::Unavailable &&
              !app.Controller.Snapshot().StartupTask,
          "a headless composition without startup integration must explicitly report it unavailable");
}

void TestRefreshPublishesAuthoritativeState() {
    CoordinatorFixture fixture;
    SnapshotRecorder recorder;
    auto token = fixture.Coordinator->Subscribe([&](auto const& snapshot) { recorder.Record(snapshot); });

    fixture.Coordinator->Refresh();
    Check(fixture.Backend->WaitForCalls(1), "refresh must start one backend query");
    auto query = fixture.Backend->GateAt(0);
    Check(query->Kind == FakeStartupTaskBackend::CallKind::Query, "refresh must call the query backend");
    Check(recorder.WaitFor([](auto const& snapshot) { return snapshot.Busy && snapshot.Revision == 1; }),
          "refresh must publish a busy revision before awaiting the backend");
    auto const busyPublication = recorder.Latest().Publication;

    fixture.Backend->Release(0, true);
    Check(recorder.WaitFor([](auto const& snapshot) {
        return snapshot.Known && snapshot.Enabled && !snapshot.Busy && !snapshot.Failed;
    }),
          "refresh must publish the authoritative enabled state");
    Check(recorder.Latest().Publication > busyPublication,
          "every stored state must carry a strictly newer publication sequence");
    Check(fixture.WaitForCommits(1), "an authoritative refresh must update the persisted cache");
    Check(fixture.CommitSnapshot() == std::vector<bool>{true}, "refresh must commit the queried state exactly once");
    fixture.Coordinator->Unsubscribe(token);
}

void TestLatestDesiredIntentRunsSeriallyAndAlonePublishes() {
    CoordinatorFixture fixture;
    SnapshotRecorder recorder;
    auto token = fixture.Coordinator->Subscribe([&](auto const& snapshot) { recorder.Record(snapshot); });

    fixture.Coordinator->RequestDesired(true);
    Check(fixture.Backend->WaitForCalls(1), "the first desired intent must start one set operation");
    Check(fixture.Backend->GateAt(0)->Kind == FakeStartupTaskBackend::CallKind::Set &&
              fixture.Backend->GateAt(0)->Desired,
          "the first set operation must preserve its desired value");

    fixture.Coordinator->RequestDesired(false);
    Check(!fixture.Backend->WaitForCalls(2, 100ms), "a replacement intent must not overlap the active backend call");
    fixture.Backend->Release(0, true);
    Check(fixture.Backend->WaitForCalls(2), "a completed set must be followed by an authoritative query");
    Check(fixture.Backend->GateAt(1)->Kind == FakeStartupTaskBackend::CallKind::Query,
          "the first desired operation must verify its actual state");
    fixture.Backend->Release(1, true);

    Check(fixture.Backend->WaitForCalls(3), "the latest desired intent must start after the old flight finishes");
    Check(fixture.Backend->GateAt(2)->Kind == FakeStartupTaskBackend::CallKind::Set &&
              !fixture.Backend->GateAt(2)->Desired,
          "only the latest replacement desired value must be executed");
    Check(fixture.CommitSnapshot().empty(), "a superseded operation must not commit its stale actual state");

    fixture.Backend->Release(2, true);
    Check(fixture.Backend->WaitForCalls(4), "the latest set must also be verified by a query");
    fixture.Backend->Release(3, false);
    Check(recorder.WaitFor([](auto const& snapshot) {
        return snapshot.Known && !snapshot.Enabled && !snapshot.Busy && !snapshot.Failed;
    }),
          "only the latest desired operation must publish its verified state");
    Check(fixture.WaitForCommits(1), "the latest verified desired state must be committed");
    Check(fixture.CommitSnapshot() == std::vector<bool>{false},
          "the superseded true result must never reach persisted state");
    fixture.Coordinator->Unsubscribe(token);
}

void TestConcurrentCallersShareOneBackendFlight() {
    CoordinatorFixture fixture;
    Check(fixture.Coordinator->RequestDesired(true), "initial request must be accepted");
    if (!fixture.Backend->WaitForCalls(1)) {
        Check(false, "initial backend flight must start");
        return;
    }
    constexpr int callers = 16;
    std::barrier start(callers);
    std::atomic<int> accepted = 0;
    std::vector<std::jthread> threads;
    for (int index = 0; index < callers; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            if (fixture.Coordinator->RequestDesired(index % 2 == 0)) ++accepted;
        });
    }
    threads.clear();
    Check(accepted == callers, "the coordinator must admit every concurrent caller");
    Check(fixture.Coordinator->RequestDesired(false), "the final explicit intent must be accepted");
    Check(fixture.Backend->CallCount() == 1, "concurrent callers must not overlap the active backend flight");
    fixture.Backend->Release(0, true);
    if (!fixture.Backend->WaitForCalls(2)) {
        Check(false, "set must be followed by query");
        return;
    }
    fixture.Backend->Release(1, true);
    if (!fixture.Backend->WaitForCalls(3)) {
        Check(false, "the latest intent must start after query");
        return;
    }
    Check(fixture.Backend->GateAt(2)->Kind == FakeStartupTaskBackend::CallKind::Set &&
              !fixture.Backend->GateAt(2)->Desired,
          "only the final false intent must follow the active flight");
    Check(fixture.CommitSnapshot().empty(), "the superseded initial result must not be persisted");
    fixture.Backend->Release(2, true);
    if (!fixture.Backend->WaitForCalls(4)) {
        Check(false, "the final set must be verified");
        return;
    }
    fixture.Backend->Release(3, false);
    Check(fixture.WaitForCommits(1), "the final verified state must be persisted");
    fixture.Coordinator->Shutdown();
    Check(fixture.CommitSnapshot() == std::vector<bool>{false}, "only the latest state may be committed");
    Check(!fixture.Coordinator->RequestDesired(true) && !fixture.Coordinator->Refresh(),
          "the coordinator alone must reject requests after shutdown");
}

void TestSameDesiredIntentCoalescesWhileRunning() {
    CoordinatorFixture fixture;
    fixture.Coordinator->RequestDesired(true);
    fixture.Coordinator->RequestDesired(true);

    Check(fixture.Backend->WaitForCalls(1), "the first same-desired request must start a set operation");
    Check(!fixture.Backend->WaitForCalls(2, 100ms), "the same desired intent must coalesce while in flight");
    fixture.Backend->Release(0, true);
    Check(fixture.Backend->WaitForCalls(2), "the coalesced set must perform only its normal verification query");
    fixture.Backend->Release(1, true);
    Check(fixture.WaitForCommits(1), "the coalesced desired intent must publish once");
    Check(fixture.CommitSnapshot().size() == 1, "same-desired coalescing must not duplicate persistence");
}

void TestReopenRefreshJoinsProcessFlightWithoutReplacingDesiredIntent() {
    CoordinatorFixture fixture;
    SnapshotRecorder firstWindow;
    auto firstToken = fixture.Coordinator->Subscribe([&](auto const& snapshot) { firstWindow.Record(snapshot); });
    fixture.Coordinator->RequestDesired(true);
    Check(fixture.Backend->WaitForCalls(1), "the first window must start the desired operation");
    fixture.Coordinator->Unsubscribe(firstToken);

    SnapshotRecorder reopenedWindow;
    auto reopenedToken = fixture.Coordinator->Subscribe([&](auto const& snapshot) { reopenedWindow.Record(snapshot); });
    fixture.Coordinator->Refresh();
    Check(!fixture.Backend->WaitForCalls(2, 100ms), "reopen refresh must not overlap the process-wide set flight");
    fixture.Backend->Release(0, true);
    Check(fixture.Backend->WaitForCalls(2), "the process-wide desired flight must verify its result");
    fixture.Backend->Release(1, true);
    Check(!fixture.Backend->WaitForCalls(3, 100ms),
          "a reopen refresh must join the authoritative desired flight without adding another query");

    Check(reopenedWindow.WaitFor([](auto const& snapshot) {
        return snapshot.Known && snapshot.Enabled && !snapshot.Busy && !snapshot.Failed;
    }),
          "the reopened window must receive the final authoritative process state");
    Check(fixture.WaitForCommits(1), "the shared desired flight must commit the final authoritative state");
    fixture.Coordinator->Unsubscribe(reopenedToken);
}

void TestBackendFailurePublishesVerifiedActualState() {
    CoordinatorFixture fixture;
    SnapshotRecorder recorder;
    auto token = fixture.Coordinator->Subscribe([&](auto const& snapshot) { recorder.Record(snapshot); });
    fixture.Coordinator->RequestDesired(true);
    Check(fixture.Backend->WaitForCalls(1), "failure test must start the set operation");
    fixture.Backend->Release(0, false, true);
    Check(fixture.Backend->WaitForCalls(2), "a failed set must still query the authoritative state");
    fixture.Backend->Release(1, false);

    Check(recorder.WaitFor([](auto const& snapshot) {
        return snapshot.Known && !snapshot.Enabled && !snapshot.Busy && snapshot.Failed;
    }),
          "a failed set must publish the verified actual state and failure flag");
    Check(fixture.WaitForCommits(1), "a known actual state after failure must repair the persisted cache");
    Check(fixture.CommitSnapshot() == std::vector<bool>{false},
          "failure recovery must commit the queried actual state rather than invert blindly");
    fixture.Coordinator->Unsubscribe(token);
}

void TestUnknownFailureRestoresLastConfirmedState() {
    CoordinatorFixture fixture;
    SnapshotRecorder recorder;
    auto token = fixture.Coordinator->Subscribe([&](auto const& snapshot) { recorder.Record(snapshot); });

    fixture.Coordinator->Refresh();
    Check(fixture.Backend->WaitForCalls(1), "confirmed-state test must start an initial query");
    fixture.Backend->Release(0, false);
    Check(recorder.WaitFor([](auto const& snapshot) {
        return snapshot.Known && !snapshot.Enabled && !snapshot.Busy && !snapshot.Failed;
    }),
          "the initial query must establish a confirmed disabled state");
    Check(fixture.WaitForCommits(1), "the initial confirmed state must be persisted");

    fixture.Coordinator->RequestDesired(true);
    Check(fixture.Backend->WaitForCalls(2), "the failed desired request must start its set operation");
    fixture.Backend->Release(1, false, true);
    Check(fixture.Backend->WaitForCalls(3), "a failed set must still attempt an authoritative query");
    fixture.Backend->Release(2, false, true);

    Check(recorder.WaitFor([](auto const& snapshot) {
        return snapshot.Known && !snapshot.Enabled && !snapshot.Busy && snapshot.Failed;
    }),
          "an unknown failure must restore the last confirmed value instead of publishing optimism");
    Check(fixture.CommitSnapshot() == std::vector<bool>{false},
          "an unknown failure must not overwrite the confirmed persisted value");
    fixture.Coordinator->Unsubscribe(token);
}

void TestShutdownDuringBusyPublicationPreventsBackendLaunch() {
    CoordinatorFixture fixture;
    auto token = fixture.Coordinator->Subscribe([coordinator = fixture.Coordinator](auto const& snapshot) {
        if (snapshot.Busy) coordinator->Shutdown();
    });

    fixture.Coordinator->RequestDesired(true);
    Check(!fixture.Backend->WaitForCalls(1, 100ms),
          "shutdown that wins during busy publication must prevent a later backend launch");
    Check(!fixture.Coordinator->Snapshot().Busy, "shutdown during publication must leave the coordinator idle");
    fixture.Coordinator->Unsubscribe(token);
}

void TestShutdownSuppressesLateCompletion() {
    CoordinatorFixture fixture;
    SnapshotRecorder recorder;
    static_cast<void>(fixture.Coordinator->Subscribe([&](auto const& snapshot) { recorder.Record(snapshot); }));
    fixture.Coordinator->Refresh();
    Check(fixture.Backend->WaitForCalls(1), "shutdown test must start a query");
    auto const notificationsBeforeShutdown = recorder.Count();

    fixture.Coordinator->Shutdown();
    fixture.Backend->Release(0, true);
    std::this_thread::sleep_for(100ms);

    Check(fixture.CommitSnapshot().empty(), "shutdown must suppress persistence from late completion");
    Check(recorder.Count() == notificationsBeforeShutdown, "shutdown must suppress late handler publication");
    Check(!fixture.Coordinator->Snapshot().Busy, "shutdown must leave the observable coordinator state idle");
}

} // namespace

int RunStartupTaskCoordinatorTests() {
    TestCommitCanReenterAndStopTheOwner();
    TestShutdownDrainsForeignCommitWithoutHoldingOwnerLock();
    TestFacadeDestructionInvalidatesPendingSetContinuation();
    TestAppControllerOwnsStartupActionsAndObservation();
    TestRefreshPublishesAuthoritativeState();
    TestLatestDesiredIntentRunsSeriallyAndAlonePublishes();
    TestSameDesiredIntentCoalescesWhileRunning();
    TestConcurrentCallersShareOneBackendFlight();
    TestReopenRefreshJoinsProcessFlightWithoutReplacingDesiredIntent();
    TestBackendFailurePublishesVerifiedActualState();
    TestUnknownFailureRestoresLastConfirmedState();
    TestShutdownDuringBusyPublicationPreventsBackendLaunch();
    TestShutdownSuppressesLateCompletion();
    return g_failures;
}
