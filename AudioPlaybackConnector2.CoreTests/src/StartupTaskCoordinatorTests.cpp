#include <app/StartupTaskCoordinator.hpp>

#include <windows.h>
#include <winrt/Windows.ApplicationModel.h>

#include <services/StartupTaskController.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class Event {
public:
    Event() : m_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!m_handle) throw std::runtime_error("CreateEventW failed");
    }
    ~Event() { CloseHandle(m_handle); }

    Event(Event const&) = delete;
    Event& operator=(Event const&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
    void Signal() const noexcept { SetEvent(m_handle); }

private:
    HANDLE m_handle = nullptr;
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

void DebugTrace(std::wstring_view) noexcept {}

winrt::Windows::Foundation::IAsyncOperation<bool> StartupTaskController::IsEnabledAsync() {
    co_return false;
}

winrt::Windows::Foundation::IAsyncOperation<bool> StartupTaskController::SetEnabledAsync(bool) {
    co_return false;
}

int RunStartupTaskCoordinatorTests() {
    TestRefreshPublishesAuthoritativeState();
    TestLatestDesiredIntentRunsSeriallyAndAlonePublishes();
    TestSameDesiredIntentCoalescesWhileRunning();
    TestReopenRefreshJoinsProcessFlightWithoutReplacingDesiredIntent();
    TestBackendFailurePublishesVerifiedActualState();
    TestUnknownFailureRestoresLastConfirmedState();
    TestShutdownDuringBusyPublicationPreventsBackendLaunch();
    TestShutdownSuppressesLateCompletion();
    return g_failures;
}
