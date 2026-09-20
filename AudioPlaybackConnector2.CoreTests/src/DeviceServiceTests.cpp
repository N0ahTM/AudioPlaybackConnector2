#include "TestCheck.hpp"
#include "DeviceTestFixture.hpp"

#include <core/DeviceService.hpp>

#include <winerror.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <iostream>
#include <future>
#include <semaphore>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// DeviceService Tests ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

using namespace apc::tests::device;

void TestSettingsPolicyRejectsOldDuplicateAndCancelledRevisions() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"policy");
    fixture.Service.ApplySettingsPolicy({10, false, false, {}, {}});
    Check(!fixture.Service.Snapshot().Sessions.front().IsReconnectEnabled,
          "the first settings policy must replace the session's initial reconnect policy");
    auto const generation = fixture.Service.Snapshot().Generation;
    fixture.Service.ApplySettingsPolicy({9, true, true, {}, {}});
    fixture.Service.ApplySettingsPolicy({10, true, true, {}, {}});
    fixture.Service.ApplySettingsPolicy({11, false, false, {}, {}});
    Check(fixture.Service.Snapshot().Generation == generation &&
              !fixture.Service.Snapshot().Sessions.front().IsIncomingEnabled,
          "old, duplicate and semantically unchanged policies must not mutate sessions or publish extra facts");
    fixture.Service.ApplySettingsPolicy({12, false, false, {L"policy"}, {}});
    Check(fixture.Service.Snapshot().Sessions.front().IsReconnectEnabled,
          "a newer per-device setting must apply independently of global reconnect");
    std::stop_source stopped;
    stopped.request_stop();
    fixture.Service.ApplySettingsPolicy({13, false, false, {}, stopped.get_token()});
    Check(fixture.Service.Snapshot().Sessions.front().IsReconnectEnabled,
          "cancelled application ownership must prevent a pending policy from changing sessions");
}

void TestSettingsPolicyDoesNotWaitForForeignDeliveryAndCancelsQueuedWork() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"queued-policy");
    std::binary_semaphore entered(0), release(0);
    bool first = true;
    auto subscription = fixture.Service.Subscribe([&](auto const&) {
        if (!std::exchange(first, false)) return;
        entered.release();
        release.acquire();
    });
    std::jthread publisher([&] { fixture.Service.ApplySettingsPolicy({1, false, false, {}, {}}); });
    entered.acquire();
    std::stop_source stopped;
    auto applying = std::async(std::launch::async,
                               [&] { fixture.Service.ApplySettingsPolicy({10, true, true, {}, stopped.get_token()}); });
    auto const returned = applying.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    stopped.request_stop();
    release.release();
    publisher.join();
    applying.get();
    fixture.Service.Unsubscribe(subscription);
    auto snapshot = fixture.Service.Snapshot();
    Check(returned && !snapshot.Sessions.front().IsIncomingEnabled && !snapshot.Sessions.front().IsReconnectEnabled,
          "settings publication must queue without waiting on a foreign observer and cancelled queued policy must be "
          "inert");
}

void TestCompletionRetainsFirstTerminalResult() {
    Fixture fixture;
    auto const command = fixture.Service.Connect(L"latched");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    connection->CompleteStart(DeviceConnectionResult::Success);
    connection->CompleteOpen(DeviceConnectionResult::Success);
    (void)fixture.Service.Disconnect(L"latched");
    CompleteCloseAndCooldown(fixture, connection);
    auto const replacement = fixture.Service.Connect(L"latched");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(fixture.Service.WaitForCompletion(command, cancelled.get_token(), std::chrono::steady_clock::now()) ==
              DeviceOperationStatus::Succeeded,
          "a completed epoch must retain success after disconnect, replacement, cancellation and deadline");
    Check(StateFor(fixture.Service, L"latched") == DeviceLifecycleState::Connecting &&
              replacement.OperationEpoch > command.OperationEpoch,
          "waiting on an old result must not cancel its replacement");
    fixture.Service.Shutdown();
    Check(fixture.Service.WaitForCompletion(replacement) == DeviceOperationStatus::Cancelled,
          "shutdown must resolve every pending operation");
    Check(fixture.Service.WaitForCompletion(command) == DeviceOperationStatus::Succeeded,
          "shutdown must not rewrite already completed operations");
}

void TestCompletionCancellationOwnershipAndDeadline() {
    Fixture fixture;
    auto const owner = fixture.Service.Connect(L"shared");
    auto const observer = fixture.Service.Connect(L"shared");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    std::stop_source cancellation;
    cancellation.request_stop();
    Check(observer.Kind == DeviceCommandResultKind::Coalesced &&
              fixture.Service.WaitForCompletion(observer, cancellation.get_token()) == DeviceOperationStatus::Cancelled,
          "a coalesced caller must be able to stop waiting independently");
    Check(connection->CloseCalls == 0 && StateFor(fixture.Service, L"shared") == DeviceLifecycleState::Connecting,
          "cancelling a coalesced wait must not cancel the accepted owner's operation");
    Check(fixture.Service.WaitForCompletion(owner, {}, std::chrono::steady_clock::now()) ==
              DeviceOperationStatus::TimedOut,
          "an elapsed deadline must return timeout and cancel the accepted operation");
    Check(connection->CloseCalls == 1, "the accepted operation's deadline must start exactly one close");
    CompleteCloseAndCooldown(fixture, connection);
    Check(fixture.Service.WaitForCompletion(observer) == DeviceOperationStatus::Cancelled,
          "the shared epoch must eventually publish cancellation to its remaining waiters");
    Fixture other;
    Check(other.Service.WaitForCompletion(owner) == DeviceOperationStatus::Rejected,
          "a completion must only be consumed through its owning device service");
}

void TestCompletionWakesWaitersAndRejectsReentrantWait() {
    Fixture fixture;
    auto const command = fixture.Service.Connect(L"wake");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    std::binary_semaphore entered(0);
    auto waiter = std::async(std::launch::async, [&] {
        entered.release();
        return fixture.Service.WaitForCompletion(
            command, {}, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    entered.acquire();
    DeviceOperationStatus reentrant = DeviceOperationStatus::Succeeded;
    auto const subscription =
        fixture.Service.Subscribe([&](DeviceFact const&) { reentrant = fixture.Service.WaitForCompletion(command); });
    connection->CompleteStart(DeviceConnectionResult::Success);
    connection->CompleteOpen(DeviceConnectionResult::Success);
    Check(waiter.get() == DeviceOperationStatus::Succeeded,
          "terminal publication must wake a waiter without a lost notification");
    Check(reentrant == DeviceOperationStatus::Rejected,
          "a publisher must not wait for work on its own serialized context");
    fixture.Service.Unsubscribe(subscription);
}

void TestCancellationDoesNotWaitBehindBlockedPublisher() {
    Fixture fixture;
    auto const command = fixture.Service.Connect(L"blocked-publication");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    std::binary_semaphore entered(0);
    std::binary_semaphore release(0);
    bool blockedOnce = false;
    auto const subscription = fixture.Service.Subscribe([&](DeviceFact const&) {
        if (blockedOnce) return;
        blockedOnce = true;
        entered.release();
        release.acquire();
    });
    // This produces a session mutation on the owned context while the accepted connect is still pending.
    std::jthread publisher([&] { fixture.Service.ApplySettingsPolicy({1, false, false, {}, {}}); });
    entered.acquire();
    std::stop_source cancellation;
    cancellation.request_stop();
    auto waiter = std::async(std::launch::async,
                             [&] { return fixture.Service.WaitForCompletion(command, cancellation.get_token()); });
    auto const completedBeforeRelease = waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.release();
    publisher.join();
    Check(completedBeforeRelease && waiter.get() == DeviceOperationStatus::Cancelled,
          "cancellation must enqueue the owned mutation without waiting behind a blocked subscriber");
    Check(connection->CloseCalls == 1, "queued cancellation must close its epoch after the subscriber returns");
    fixture.Service.Unsubscribe(subscription);
}

void TestOperationEpochRejectsStaleCompletion() {
    Fixture fixture;
    (void)fixture.Service.Connect(L"epoch");
    auto* const first = fixture.ConnectionAccess->LastConnection;
    (void)fixture.Service.Disconnect(L"epoch");
    first->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"epoch") == DeviceLifecycleState::Disconnecting,
          "a stale start completion must not reopen a disconnecting session");
    CompleteCloseAndCooldown(fixture, first);
    Check(StateFor(fixture.Service, L"epoch") == DeviceLifecycleState::Idle,
          "the current close completion must settle the session to idle");
}

void TestDuplicateConnectCoalescesWithoutReplacingConnectedSession() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"duplicate-connect");
    auto* const existingConnection = fixture.ConnectionAccess->LastConnection;
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();
    auto const operationEpoch = SessionFor(fixture.Service, L"duplicate-connect").OperationEpoch;

    auto const duplicate = fixture.Service.Connect(L"duplicate-connect");
    Check(duplicate.Command == DeviceCommandKind::Connect && duplicate.Kind == DeviceCommandResultKind::Coalesced &&
              duplicate.DeviceId == L"duplicate-connect" && duplicate.OperationEpoch == operationEpoch,
          "a duplicate connect for an open session must return the coalesced operation without advancing its epoch");
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount &&
              fixture.ConnectionAccess->LastConnection == existingConnection && existingConnection->CloseCalls == 0,
          "a duplicate connect must retain the existing open connection without beginning a replacement");
    Check(StateFor(fixture.Service, L"duplicate-connect") == DeviceLifecycleState::Connected &&
              SessionFor(fixture.Service, L"duplicate-connect").HasConnection,
          "a duplicate connect must preserve the connected session snapshot");

    auto const reconnect = fixture.Service.Reconnect(L"duplicate-connect");
    Check(reconnect.Command == DeviceCommandKind::Reconnect && reconnect.Kind == DeviceCommandResultKind::Accepted &&
              existingConnection->CloseCalls == 1,
          "an explicit reconnect must remain a replacement operation for an open session");
    existingConnection->CompleteClose();
    fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1 &&
              fixture.ConnectionAccess->LastConnection != existingConnection,
          "an explicit reconnect must create a replacement after the close barrier");
}

void TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.WatcherAccess->LastWatcher->Add(L"removed", L"Removed");
    ConnectSuccessfully(fixture, L"removed");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    const auto connectionCount = fixture.ConnectionAccess->Connections.size();

    fixture.WatcherAccess->LastWatcher->Remove(L"removed");
    Check(connection->CloseCalls == 1, "device removal must close the retained connection exactly once");
    connection->CompleteStart(DeviceConnectionResult::Success);
    connection->Signal(DeviceConnectionState::Opened);
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount,
          "late callbacks from a removed device must not create a replacement connection");
    CompleteCloseAndCooldown(fixture, connection);
    Check(StateFor(fixture.Service, L"removed") == DeviceLifecycleState::WaitingForReconnect &&
              !SessionFor(fixture.Service, L"removed").IsReconnectCancelled,
          "device removal must preserve loss recovery without recording explicit user cancellation");

    fixture.WatcherAccess->LastWatcher->Add(L"removed", L"Removed Again");
    Check(SessionFor(fixture.Service, L"removed").DeviceName == L"Removed Again",
          "reappearing devices must update the retained session snapshot");
    fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"removed") == DeviceLifecycleState::Connected,
          "a returned device must recover through the retained reconnect policy");
}

void TestRemovingAnIdleDiscoveredDeviceDoesNotPublishTerminalFailure() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.WatcherAccess->LastWatcher->Add(L"idle-removed", L"Idle removed");
    fixture.Facts.clear();

    fixture.WatcherAccess->LastWatcher->Remove(L"idle-removed");

    Check(StateFor(fixture.Service, L"idle-removed") == DeviceLifecycleState::Idle,
          "removing an idle discovered device must retain its idle session state");
    Check(!std::ranges::any_of(
              fixture.Facts,
              [](DeviceFact const& fact) { return fact.DeviceId == L"idle-removed" && fact.IsTerminalFailure; }),
          "removing an idle discovered device must not publish a terminal failure fact");
}

void TestPlatformSetupExceptionsCleanUpAndPublishTerminalFacts() {
    auto checkFailure = [](Fixture& fixture, std::wstring_view deviceId, std::string_view context) {
        auto const session = SessionFor(fixture.Service, deviceId);
        Check(session.State == DeviceLifecycleState::Failed && !session.HasConnection,
              "a platform setup exception must leave no retained connecting connection");
        Check(std::ranges::any_of(fixture.Facts,
                                  [deviceId](DeviceFact const& fact) {
                                      return fact.DeviceId == deviceId && fact.IsTerminalFailure &&
                                             fact.ConnectionResult == DeviceConnectionResult::Failed;
                                  }),
              context);
    };

    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnRegister = true;
        (void)fixture.Service.Connect(L"register-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        Check(connection->CloseCalls == 1, "handler registration failure must close the created connection once");
        CompleteCloseAndCooldown(fixture, connection);
        checkFailure(fixture, L"register-throws", "handler registration failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
        (void)fixture.Service.Connect(L"start-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        Check(connection->RevokeCalls == 1 && connection->CloseCalls == 1,
              "Start failure must revoke its token and close the created connection once");
        CompleteCloseAndCooldown(fixture, connection);
        checkFailure(fixture, L"start-throws", "Start failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnOpen = true;
        (void)fixture.Service.Connect(L"open-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        Check(connection->RevokeCalls == 1 && connection->CloseCalls == 1,
              "Open failure must revoke its token and close the created connection once");
        CompleteCloseAndCooldown(fixture, connection);
        checkFailure(fixture, L"open-throws", "Open failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"retry-timer-throws");
        fixture.TimerAccess->ThrowNextSchedule = true;
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        checkFailure(
            fixture, L"retry-timer-throws", "reconnect timer setup failure must publish a terminal failure fact");
    }
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"close-timer-throws");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const createCount = fixture.ConnectionAccess->Connections.size();
        fixture.TimerAccess->ThrowNextSchedule = true;
        (void)fixture.Service.Reconnect(L"close-timer-throws");
        Check(connection->CloseCalls == 1 && fixture.ConnectionAccess->Connections.size() == createCount,
              "close timer setup failure must retain its close barrier until close completion is confirmed");
        connection->CompleteClose();
        CompleteCloseAndCooldown(fixture, connection);
        Check(
            fixture.ConnectionAccess->Connections.size() == createCount &&
                StateFor(fixture.Service, L"close-timer-throws") == DeviceLifecycleState::Idle,
            "a late close completion after timer setup failure must settle without starting the abandoned replacement");
        Check(std::ranges::any_of(fixture.Facts,
                                  [](DeviceFact const& fact) {
                                      return fact.DeviceId == L"close-timer-throws" && fact.IsTerminalFailure &&
                                             fact.ConnectionResult == DeviceConnectionResult::Failed;
                                  }),
              "close timer setup failure must publish a terminal failure fact");
    }
}

void TestReentrantCommandsRetainDeviceIdentity() {
    Fixture fixture;
    bool observed = false;
    auto const subscription = fixture.Service.Subscribe([&](DeviceFact const& fact) {
        if (fact.Kind != DeviceFactKind::InventoryChanged || observed) return;
        observed = true;
        std::array results{fixture.Service.Connect(L"queued-connect"),
                           fixture.Service.Disconnect(L"queued-disconnect"),
                           fixture.Service.Reconnect(L"queued-reconnect"),
                           fixture.Service.CancelReconnect(L"queued-cancel")};
        std::array expected{L"queued-connect", L"queued-disconnect", L"queued-reconnect", L"queued-cancel"};
        for (std::size_t index = 0; index < results.size(); ++index) {
            Check(results[index].Kind == DeviceCommandResultKind::Coalesced &&
                      results[index].DeviceId == expected[index],
                  "a reentrant queued command must retain its target in the immediate result");
        }
    });
    (void)fixture.Service.Start();
    Check(observed, "the command fixture must execute inside the serialized publisher");
    fixture.Service.Unsubscribe(subscription);
}

void TestConcurrentCommandWaitsForSerializedMutation() {
    Fixture fixture;
    std::mutex gateMutex;
    std::condition_variable gate;
    bool factSinkEntered = false;
    bool releaseFactSink = false;

    (void)fixture.Service.Subscribe([&](DeviceFact const& fact) {
        if (fact.Kind != DeviceFactKind::InventoryChanged) return;
        std::unique_lock lock(gateMutex);
        if (factSinkEntered) return;
        factSinkEntered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return releaseFactSink; });
    });

    apc::device::DeviceCommandResult concurrentResult;
    std::jthread concurrentCaller([&] {
        {
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [&] { return factSinkEntered; });
        }
        concurrentResult = fixture.Service.Connect(L"concurrent");
    });
    std::jthread releaseCaller([&] {
        {
            std::unique_lock lock(gateMutex);
            gate.wait(lock, [&] { return factSinkEntered; });
        }
        {
            std::lock_guard lock(gateMutex);
            releaseFactSink = true;
        }
        gate.notify_all();
    });

    const auto startResult = fixture.Service.Start();
    concurrentCaller.join();
    releaseCaller.join();

    Check(startResult.Kind == DeviceCommandResultKind::Accepted,
          "the first serialized command must complete before a concurrent command");
    Check(concurrentResult.Kind == DeviceCommandResultKind::Accepted && concurrentResult.DeviceId == L"concurrent",
          "a concurrent command must wait for the serialized context and receive its actual result");
}

void TestStopAndShutdownReturnNormalizedTerminalResults() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "start must establish watcher ownership");
    auto const stop = fixture.Service.Stop();
    Check(stop.Command == DeviceCommandKind::Stop && stop.Kind == DeviceCommandResultKind::Accepted,
          "stop must report the Stop command kind rather than Start");
    fixture.Service.Shutdown();
    auto const snapshot = fixture.Service.Snapshot();
    Check(snapshot.IsShutdown && snapshot.Sessions.empty() && !snapshot.IsRunning,
          "post-shutdown snapshots must retain the terminal shutdown state");
}

void TestFactsCarryNormalizedSnapshots() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"z");
    ConnectSuccessfully(fixture, L"a");
    auto const hasSortedSnapshot = std::ranges::any_of(fixture.Facts, [](DeviceFact const& fact) {
        return fact.Kind == DeviceFactKind::SessionChanged && fact.Snapshot.Sessions.size() == 2 &&
               fact.Snapshot.Sessions[0].DeviceId == L"a" && fact.Snapshot.Sessions[1].DeviceId == L"z";
    });
    Check(hasSortedSnapshot, "typed facts must contain normalized, deterministically ordered snapshots");
}

void TestFailureFactsRetainOperationKind() {
    {
        Fixture fixture;
        fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
        (void)fixture.Service.Connect(L"manual-fact-operation");
        CompleteCloseAndCooldown(fixture, fixture.ConnectionAccess->LastConnection);
        Check(std::ranges::any_of(fixture.Facts,
                                  [](DeviceFact const& fact) {
                                      return fact.DeviceId == L"manual-fact-operation" && fact.IsTerminalFailure &&
                                             fact.Operation == apc::device::DeviceOperationKind::ManualConnect;
                                  }),
              "manual terminal failures must retain ManualConnect through the service fact");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"automatic-fact-operation");
        fixture.Service.ApplySettingsPolicy({1, false, false, {L"automatic-fact-operation"}, {}});
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
        fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
        auto* const automaticConnection = fixture.ConnectionAccess->LastConnection;
        automaticConnection->CompleteClose();
        fixture.TimerAccess->ThrowNextSchedule = true;
        fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
        Check(std::ranges::any_of(fixture.Facts,
                                  [](DeviceFact const& fact) {
                                      return fact.DeviceId == L"automatic-fact-operation" && fact.IsTerminalFailure &&
                                             fact.Operation == apc::device::DeviceOperationKind::AutomaticReconnect;
                                  }),
              "automatic terminal failures must retain AutomaticReconnect through the service fact");
    }
}

void TestDisconnectReasonsSelectTheLockedNotificationPolicy() {
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"normal-disconnect");
        fixture.Facts.clear();
        (void)fixture.Service.Disconnect(L"normal-disconnect");

        auto const normal = std::ranges::find_if(fixture.Facts, [](DeviceFact const& fact) {
            return fact.DeviceId == L"normal-disconnect" && fact.DisconnectReason == DeviceDisconnectReason::Normal;
        });
        Check(normal != fixture.Facts.end() && normal->Kind == DeviceFactKind::SessionChanged,
              "an explicit disconnect must remain a typed session fact");
        Check(normal != fixture.Facts.end() && normal->DisconnectReason != DeviceDisconnectReason::UnexpectedLoss &&
                  normal->DisconnectReason != DeviceDisconnectReason::DeviceRemoved,
              "normal disconnect policy must suppress toast and fallback notification");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"unexpected-loss");
        fixture.Service.ApplySettingsPolicy({1, false, false, {}, {}});
        fixture.Facts.clear();
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);

        auto const unexpected = std::ranges::find_if(fixture.Facts, [](DeviceFact const& fact) {
            return fact.DeviceId == L"unexpected-loss" &&
                   fact.DisconnectReason == DeviceDisconnectReason::UnexpectedLoss;
        });
        Check(unexpected != fixture.Facts.end() && unexpected->Kind == DeviceFactKind::SessionChanged &&
                  unexpected->IsTerminalFailure,
              "a terminal unexpected loss must remain a typed disconnect fact");
        Check(unexpected != fixture.Facts.end() &&
                  unexpected->DisconnectReason == DeviceDisconnectReason::UnexpectedLoss,
              "an unexpected loss must retain the notifying disconnect policy");
        Check(!std::ranges::any_of(fixture.Facts,
                                   [](DeviceFact const& fact) {
                                       return fact.DeviceId == L"unexpected-loss" &&
                                              fact.Kind == DeviceFactKind::OperationFailed && fact.IsTerminalFailure;
                                   }),
              "terminal unexpected loss must not synthesize an operation-error fact");
    }
}

void TestOperationFailuresRemainOperationFailuresWithoutDisconnectReason() {
    Fixture fixture;
    fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
    (void)fixture.Service.Connect(L"operation-failure");

    Check(std::ranges::any_of(fixture.Facts,
                              [](DeviceFact const& fact) {
                                  return fact.DeviceId == L"operation-failure" &&
                                         fact.Kind == DeviceFactKind::OperationFailed &&
                                         fact.DisconnectReason == DeviceDisconnectReason::None;
                              }),
          "connection setup failures must retain the operation-error fact category");
}

} // namespace

int RunDeviceServiceTests() {
    TestSettingsPolicyRejectsOldDuplicateAndCancelledRevisions();
    TestSettingsPolicyDoesNotWaitForForeignDeliveryAndCancelsQueuedWork();
    TestCompletionRetainsFirstTerminalResult();
    TestCompletionCancellationOwnershipAndDeadline();
    TestCompletionWakesWaitersAndRejectsReentrantWait();
    TestCancellationDoesNotWaitBehindBlockedPublisher();
    TestOperationEpochRejectsStaleCompletion();
    TestDuplicateConnectCoalescesWithoutReplacingConnectedSession();
    TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks();
    TestRemovingAnIdleDiscoveredDeviceDoesNotPublishTerminalFailure();
    TestPlatformSetupExceptionsCleanUpAndPublishTerminalFacts();
    TestReentrantCommandsRetainDeviceIdentity();
    TestConcurrentCommandWaitsForSerializedMutation();
    TestStopAndShutdownReturnNormalizedTerminalResults();
    TestFactsCarryNormalizedSnapshots();
    TestFailureFactsRetainOperationKind();
    TestDisconnectReasonsSelectTheLockedNotificationPolicy();
    TestOperationFailuresRemainOperationFailuresWithoutDisconnectReason();
    return g_failures;
}
