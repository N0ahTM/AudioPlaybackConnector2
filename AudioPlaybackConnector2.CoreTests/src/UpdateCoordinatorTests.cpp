#include <services/UpdateCoordinator.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
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

    [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
    void Signal() const noexcept { SetEvent(m_handle); }

private:
    HANDLE m_handle = nullptr;
};

class FakeUpdateBackend {
public:
    struct Gate {
        Event Completed;
        std::mutex Mutex;
        UpdateCheckResult Result{UpdateCheckStatus::UpToDate, L"1.0.0", L"1.0.0"};
        bool IgnoreCancellation = false;
    };

    UpdateCheckTask CheckAsync(std::stop_token stopToken) {
        auto gate = std::make_shared<Gate>();
        gate->IgnoreCancellation = IgnoreCancellation.load();
        {
            std::lock_guard lock(m_mutex);
            m_gates.push_back(gate);
            ++m_callCount;
        }
        m_changed.notify_all();

        std::stop_callback cancellation(stopToken, [this, gate]() noexcept {
            ++m_cancellationCount;
            m_changed.notify_all();
            if (!gate->IgnoreCancellation) gate->Completed.Signal();
        });
        co_await winrt::resume_on_signal(gate->Completed.Get());

        if (stopToken.stop_requested() && !gate->IgnoreCancellation) {
            UpdateCheckResult cancelled;
            cancelled.Status = UpdateCheckStatus::Cancelled;
            co_return cancelled;
        }
        std::lock_guard lock(gate->Mutex);
        co_return gate->Result;
    }

    bool WaitForCalls(size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&]() { return m_callCount >= count; });
    }

    bool WaitForCancellations(size_t count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&]() { return m_cancellationCount.load() >= count; });
    }

    void Release(size_t index, UpdateCheckStatus status = UpdateCheckStatus::UpToDate) {
        std::shared_ptr<Gate> gate;
        {
            std::lock_guard lock(m_mutex);
            gate = m_gates.at(index);
        }
        {
            std::lock_guard lock(gate->Mutex);
            gate->Result.Status = status;
        }
        gate->Completed.Signal();
    }

    [[nodiscard]] size_t Calls() const noexcept { return m_callCount.load(); }
    [[nodiscard]] size_t Cancellations() const noexcept { return m_cancellationCount.load(); }

    std::atomic_bool IgnoreCancellation = false;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::shared_ptr<Gate>> m_gates;
    std::atomic_size_t m_callCount = 0;
    std::atomic_size_t m_cancellationCount = 0;
};

std::shared_ptr<UpdateCoordinator> MakeCoordinator(std::shared_ptr<FakeUpdateBackend> const& backend) {
    return std::make_shared<UpdateCoordinator>(
        [backend](std::stop_token stopToken) { return backend->CheckAsync(stopToken); });
}

winrt::Windows::Foundation::IAsyncAction
AwaitCheck(std::shared_ptr<UpdateCoordinator> coordinator, UpdateCheckReason reason, UpdateCheckResult& result) {
    result = co_await coordinator->CheckForUpdatesAsync(reason);
}

winrt::Windows::Foundation::IAsyncAction AwaitAutomaticWindow(std::shared_ptr<UpdateCoordinator> coordinator,
                                                              bool& result) {
    result = co_await coordinator->WaitForAutomaticCheckWindowAsync(1h);
}

void TestSingleFlightAndAutomaticResultReuse() {
    auto backend = std::make_shared<FakeUpdateBackend>();
    auto coordinator = MakeCoordinator(backend);
    UpdateCheckResult automaticResult;
    UpdateCheckResult manualResult;
    auto automatic = AwaitCheck(coordinator, UpdateCheckReason::Automatic, automaticResult);
    Check(backend->WaitForCalls(1), "automatic update check must start its backend flight");
    auto manual = AwaitCheck(coordinator, UpdateCheckReason::Manual, manualResult);
    Check(backend->Calls() == 1, "manual and automatic callers must share one in-flight request");
    backend->Release(0);
    automatic.get();
    manual.get();
    Check(automaticResult.Status == UpdateCheckStatus::UpToDate && manualResult.Status == UpdateCheckStatus::UpToDate,
          "all joined callers must receive the same successful result");

    UpdateCheckResult reusedResult;
    AwaitCheck(coordinator, UpdateCheckReason::Automatic, reusedResult).get();
    Check(backend->Calls() == 1, "a recent successful result must satisfy an automatic check without new I/O");
    Check(reusedResult.Status == UpdateCheckStatus::UpToDate, "the reused automatic result must remain intact");

    UpdateCheckResult freshManualResult;
    auto freshManual = AwaitCheck(coordinator, UpdateCheckReason::Manual, freshManualResult);
    Check(backend->WaitForCalls(2), "a manual check must bypass the automatic result cache");
    backend->Release(1, UpdateCheckStatus::UpdateAvailable);
    freshManual.get();
    Check(freshManualResult.Status == UpdateCheckStatus::UpdateAvailable,
          "manual cache bypass must return the fresh backend result");
}

void TestManualWaiterProtectsAutomaticFlightFromPolicyCancellation() {
    auto backend = std::make_shared<FakeUpdateBackend>();
    auto coordinator = MakeCoordinator(backend);
    UpdateCheckResult automaticResult;
    UpdateCheckResult manualResult;
    auto automatic = AwaitCheck(coordinator, UpdateCheckReason::Automatic, automaticResult);
    Check(backend->WaitForCalls(1), "protected automatic flight must start");
    auto manual = AwaitCheck(coordinator, UpdateCheckReason::Manual, manualResult);

    coordinator->SetAutomaticChecksAllowed(false);
    Check(backend->Cancellations() == 0, "a manual waiter must upgrade and protect the shared automatic flight");
    backend->Release(0);
    automatic.get();
    manual.get();
    Check(automaticResult.Status == UpdateCheckStatus::UpToDate && manualResult.Status == UpdateCheckStatus::UpToDate,
          "policy changes must not discard a manually requested shared result");
}

void TestStableAutomaticWindowDeterministically() {
    AutomaticUpdateWindow window;
    const auto at = [](int seconds) { return AutomaticUpdateWindow::TimePoint{} + std::chrono::seconds(seconds); };

    Check(!window.Update(true, false, at(0), 10s), "stable window must not open immediately");
    Check(window.Remaining(at(4), 10s) == 6s, "stable window must report its exact remaining delay");
    Check(!window.Update(true, false, at(9), 10s), "stable window must honor the complete delay");
    Check(window.Update(true, false, at(10), 10s), "stable window must open exactly at the delay boundary");
    Check(!window.Update(false, false, at(11), 10s), "disallowing automatic work must reset the stable window");
    Check(!window.Update(true, false, at(20), 10s), "a new allowed period must restart the stable delay");
    Check(window.Update(true, false, at(30), 10s), "the restarted stable period must eventually open");
    Check(!window.Update(true, true, at(31), 10s), "shutdown must close and reset the stable window");
    Check(!window.Update(true, false, at(5), 10s), "clock rollback must never open the window early");
}

void TestShutdownAndLateCompletion() {
    auto backend = std::make_shared<FakeUpdateBackend>();
    backend->IgnoreCancellation = true;
    auto coordinator = MakeCoordinator(backend);
    UpdateCheckResult result;
    auto action = AwaitCheck(coordinator, UpdateCheckReason::Automatic, result);
    Check(backend->WaitForCalls(1), "late-completion flight must start");

    coordinator->Shutdown();
    Check(backend->WaitForCancellations(1), "shutdown must request cancellation immediately");
    backend->Release(0, UpdateCheckStatus::UpdateAvailable);
    action.get();
    Check(result.Status == UpdateCheckStatus::Cancelled,
          "a successful backend completion arriving after shutdown must be reported as cancelled");

    UpdateCheckResult rejected;
    AwaitCheck(coordinator, UpdateCheckReason::Automatic, rejected).get();
    Check(rejected.Status == UpdateCheckStatus::Cancelled && backend->Calls() == 1,
          "shutdown must reject all later checks without starting backend work");
}

void TestShutdownWakesEveryAutomaticWindowWaiter() {
    constexpr size_t waiterCount = 16;
    auto backend = std::make_shared<FakeUpdateBackend>();
    auto coordinator = MakeCoordinator(backend);
    std::array<bool, waiterCount> results{};
    results.fill(true);
    std::vector<winrt::Windows::Foundation::IAsyncAction> waiters;
    waiters.reserve(waiterCount);

    for (size_t index = 0; index < waiterCount; ++index) {
        waiters.push_back(AwaitAutomaticWindow(coordinator, results[index]));
    }

    coordinator->Shutdown();
    for (auto const& waiter : waiters)
        waiter.get();
    Check(std::ranges::none_of(results, [](bool result) { return result; }),
          "shutdown must wake and cancel every automatic-window waiter");
}

void TestParallelSingleFlightStress() {
    constexpr size_t callerCount = 32;
    auto backend = std::make_shared<FakeUpdateBackend>();
    auto coordinator = MakeCoordinator(backend);
    std::barrier start(static_cast<std::ptrdiff_t>(callerCount + 1));
    std::atomic_size_t prepared = 0;
    std::atomic_bool releaseCallers = false;
    std::vector<UpdateCheckResult> results(callerCount);
    std::vector<std::jthread> callers;
    callers.reserve(callerCount);

    for (size_t index = 0; index < callerCount; ++index) {
        callers.emplace_back([&, index]() {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            start.arrive_and_wait();
            auto action = AwaitCheck(coordinator, UpdateCheckReason::Manual, results[index]);
            prepared.fetch_add(1);
            prepared.notify_all();
            while (!releaseCallers.load())
                releaseCallers.wait(false);
            action.get();
            winrt::uninit_apartment();
        });
    }

    start.arrive_and_wait();
    auto preparedCount = prepared.load();
    while (preparedCount != callerCount) {
        prepared.wait(preparedCount);
        preparedCount = prepared.load();
    }
    Check(backend->WaitForCalls(1), "parallel callers must start one backend request");
    Check(backend->Calls() == 1, "all parallel callers must join exactly one flight");
    backend->Release(0);
    releaseCallers = true;
    releaseCallers.notify_all();
    callers.clear();

    Check(std::ranges::all_of(results, [](auto const& result) { return result.Status == UpdateCheckStatus::UpToDate; }),
          "all parallel callers must receive the shared result");
    Check(backend->Calls() == 1, "parallel completion must not create follow-up flights");
}
} // namespace

int RunUpdateCoordinatorTests() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestSingleFlightAndAutomaticResultReuse();
    TestManualWaiterProtectsAutomaticFlightFromPolicyCancellation();
    TestStableAutomaticWindowDeterministically();
    TestShutdownAndLateCompletion();
    TestShutdownWakesEveryAutomaticWindowWaiter();
    TestParallelSingleFlightStress();
    winrt::uninit_apartment();
    return g_failures;
}
