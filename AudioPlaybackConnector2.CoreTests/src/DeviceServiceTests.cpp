#include "TestCheck.hpp"
#include "AppTestFixture.hpp"

#include <app/AppController.hpp>
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

namespace {

using namespace apc::tests::device;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Operation Completion //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

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
    std::jthread publisher([&] { fixture.Service.SetReconnectOnConnectionLoss(L"blocked-publication", true); });
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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Controller Device Events //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestControllerOwnsDeviceFactNormalization() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    auto settings = apc::tests::MakeTestSettings();
    (void)settings->SetGlobalReconnectOnConnectionLoss(true);
    apc::app::AppController controller(settings, std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller.Subscribe([&](auto const& event) { events.push_back(event); });
    const auto longId = std::wstring(513, L'd');
    ConnectSuccessfully(*fixture, longId);
    auto const connected = std::ranges::find_if(
        events, [](auto const& event) { return std::holds_alternative<apc::app::DeviceConnectedEvent>(event.Event); });
    Check(connected != events.end(), "the controller must normalize the concrete owner's connected fact");
    if (connected == events.end()) return;
    auto const retainedConnected = *connected;
    Check(std::get<apc::app::DeviceConnectedEvent>(retainedConnected.Event).Id.View() == longId &&
              controller.IsCurrent(retainedConnected),
          "controller events must preserve opaque external identities and current delivery tokens");
    (void)fixture->Service.Disconnect(longId);
    Check(!controller.IsCurrent(retainedConnected),
          "a queued connected UI event must become stale when the session disconnects");
    auto const manual = std::ranges::find_if(events, [](auto const& event) {
        return std::holds_alternative<apc::app::DeviceDisconnectedEvent>(event.Event);
    });
    Check(manual != events.end() && !std::get<apc::app::DeviceDisconnectedEvent>(manual->Event).NotifyUser,
          "manual disconnect events must not request a loss notification");
    CompleteCloseAndCooldown(*fixture, fixture->ConnectionAccess->LastConnection);
    ConnectSuccessfully(*fixture, longId);
    fixture->ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  auto const* disconnected =
                                      std::get_if<apc::app::DeviceDisconnectedEvent>(&event.Event);
                                  return disconnected && disconnected->NotifyUser;
                              }),
          "unexpected loss must carry the notification policy in the typed application event");
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  return std::holds_alternative<apc::app::AutoReconnectTriggeredEvent>(event.Event);
                              }),
          "entering reconnect wait must produce the typed reconnect event");
    for (std::size_t i = 1; i < events.size(); ++i)
        Check(events[i - 1].Revision < events[i].Revision,
              "device publications must share the controller's total revision order");
}

void TestControllerStatusTokensFollowTheDeviceOwner() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    apc::app::AppController controller(apc::tests::MakeTestSettings(),
                                       std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller.Subscribe([&](auto const& event) { events.push_back(event); });
    auto lastStatus = [&]() -> std::optional<apc::app::AppController::EventNotification> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (std::holds_alternative<apc::app::DeviceStatusChangedEvent>(it->Event)) return *it;
        return std::nullopt;
    };
    (void)fixture->Service.Start();
    fixture->WatcherAccess->LastWatcher->Add(L"status-token", L"Status token");
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  return std::holds_alternative<apc::app::DeviceInventoryChangedEvent>(event.Event);
                              }) &&
              std::ranges::any_of(events,
                                  [](auto const& event) {
                                      return std::holds_alternative<apc::app::DeviceActivityChangedEvent>(event.Event);
                                  }),
          "inventory and activity must use the same typed controller subscription");

    (void)fixture->Service.Connect(L"status-token");
    auto const connecting = lastStatus();
    fixture->Service.SetReconnectOnConnectionLoss(L"status-token", true);
    auto const duplicate = lastStatus();
    Check(connecting && duplicate && connecting->Revision < duplicate->Revision && controller.IsCurrent(*connecting) &&
              controller.IsCurrent(*duplicate),
          "equal source statuses must retain current tokens while preserving distinct publication revisions");
    auto* const connection = fixture->ConnectionAccess->LastConnection;
    connection->CompleteStart(DeviceConnectionResult::Denied);
    CompleteCloseAndCooldown(*fixture, connection);
    auto const failed = lastStatus();
    Check(connecting && duplicate && failed && !controller.IsCurrent(*connecting) &&
              !controller.IsCurrent(*duplicate) && controller.IsCurrent(*failed) &&
              std::get<apc::app::DeviceStatusChangedEvent>(failed->Event).State ==
                  apc::app::DeviceConnectionState::Failed,
          "terminal failure must invalidate queued connecting statuses before UI delivery");

    (void)fixture->Service.Connect(L"status-token");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture->ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(failed && !controller.IsCurrent(*failed), "a replacement connection must invalidate a queued failure event");
    fixture->Service.SetReconnectOnConnectionLoss(L"status-token", true);
    Check(std::ranges::count_if(events,
                                [](auto const& event) {
                                    return std::holds_alternative<apc::app::DeviceConnectedEvent>(event.Event);
                                }) == 1,
          "repeated connected status must not emit a second connected transition");
    Check(StateFor(fixture->Service, L"status-token") == DeviceLifecycleState::Connected,
          "retained old notifications must not change the authoritative session state");
}

void TestControllerDeviceFailureAndLifetime() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    auto controller = std::make_unique<apc::app::AppController>(
        apc::tests::MakeTestSettings(), std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller->Subscribe([&](auto const& event) { events.push_back(event); });
    (void)fixture->Service.Connect(L"denied");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Denied);
    CompleteCloseAndCooldown(*fixture, fixture->ConnectionAccess->LastConnection);
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  auto const* error = std::get_if<apc::app::DeviceConnectionErrorEvent>(&event.Event);
                                  return error &&
                                         error->FailureReason == apc::app::DeviceConnectionErrorEvent::Reason::Denied;
                              }),
          "the application must carry a typed failure reason without loading localized UI resources");
    controller.reset();
    auto const count = events.size();
    (void)fixture->Service.Connect(L"after-controller");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture->ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(events.size() == count,
          "controller destruction must detach the source and make retained callbacks ineffective");
    subscription.Reset();
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

void TestReconnectWaitsForCloseAndRevokesTheOldToken() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"barrier");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();
    (void)fixture.Service.Reconnect(L"barrier");
    Check(oldConnection->RevokeCalls == 1 && oldConnection->CloseCalls == 1,
          "reconnect must revoke the old state token before closing it");
    Check(fixture.ConnectionAccess->Connections.size() == createCount,
          "a replacement must not be created before the close barrier completes");
    oldConnection->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == createCount,
          "a close completion must retain the barrier during the required cooldown");
    auto* const cooldown = fixture.TimerAccess->LastTimer;
    Check(cooldown->Delay == std::chrono::milliseconds(1500), "a completed close must retain the cooldown barrier");
    cooldown->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == createCount + 1,
          "the close completion must be the only transition that starts replacement creation");
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

void TestCloseBarrierTimeoutRetainsTheOldConnectionUntilLateCompletion() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"close-timeout");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();

    (void)fixture.Service.Reconnect(L"close-timeout");
    (void)fixture.Service.Reconnect(L"close-timeout");
    auto* const closeBarrierTimer = fixture.TimerAccess->LastTimer;
    Check(oldConnection->CloseCalls == 1, "a close barrier must issue exactly one close while it is in flight");
    Check(closeBarrierTimer->Delay == std::chrono::seconds(5), "the close barrier must retain a bounded cooldown");
    closeBarrierTimer->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == createCount &&
              StateFor(fixture.Service, L"close-timeout") == DeviceLifecycleState::Failed,
          "a close timeout must be terminal while retaining the old connection barrier without a replacement");
    auto const timedOutEpoch = SessionFor(fixture.Service, L"close-timeout").OperationEpoch;
    auto const disconnect = fixture.Service.Disconnect(L"close-timeout");
    Check(
        disconnect.Kind == DeviceCommandResultKind::Coalesced &&
            StateFor(fixture.Service, L"close-timeout") == DeviceLifecycleState::Failed &&
            SessionFor(fixture.Service, L"close-timeout").OperationEpoch == timedOutEpoch &&
            !fixture.Service.HasBusyOperations(),
        "a disconnect after an abandoned close must retain the terminal state without a stuck Disconnecting operation");
    (void)fixture.Service.DisconnectAll();
    Check(StateFor(fixture.Service, L"close-timeout") == DeviceLifecycleState::Failed &&
              SessionFor(fixture.Service, L"close-timeout").OperationEpoch == timedOutEpoch,
          "bulk and internal disconnect callers must retain an abandoned close's terminal state");
    auto reconnectAfterDisconnect = fixture.Service.Reconnect(L"close-timeout");
    Check(fixture.Service.WaitForCompletion(reconnectAfterDisconnect) == DeviceOperationStatus::Failed,
          "an reconnect after an abandoned close and explicit disconnect must complete with the terminal failure");
    (void)fixture.Service.Reconnect(L"close-timeout");
    Check(fixture.ConnectionAccess->Connections.size() == createCount,
          "a command issued while a timed-out close remains unconfirmed must not create a replacement");
    oldConnection->CompleteClose();
    Check(oldConnection->CloseCalls == 1 && fixture.ConnectionAccess->Connections.size() == createCount,
          "late close completion must retain the normal cooldown without creating the abandoned replacement");
    auto* const cooldown = fixture.TimerAccess->LastTimer;
    Check(cooldown->Delay == std::chrono::milliseconds(1500),
          "a late completion after timeout must enter the normal close cooldown");
    cooldown->FireEvenIfCancelled();
    Check(StateFor(fixture.Service, L"close-timeout") == DeviceLifecycleState::Idle &&
              fixture.ConnectionAccess->Connections.size() == createCount,
          "the late completion must settle the abandoned operation without silently reconnecting");
    oldConnection->CompleteClose();
    Check(StateFor(fixture.Service, L"close-timeout") == DeviceLifecycleState::Idle &&
              fixture.ConnectionAccess->Connections.size() == createCount,
          "a duplicate late close callback after an abandoned barrier must be ignored");
    (void)fixture.Service.Reconnect(L"close-timeout");
    Check(fixture.ConnectionAccess->Connections.size() == createCount + 1,
          "a later explicit reconnect may start only after the close barrier has completed");
    Check(std::ranges::any_of(fixture.Facts,
                              [](DeviceFact const& fact) {
                                  return fact.DeviceId == L"close-timeout" && fact.IsTerminalFailure &&
                                         fact.ConnectionResult == DeviceConnectionResult::TimedOut;
                              }),
          "the close timeout must publish a deterministic terminal timeout fact");
}

void TestCloseBarrierTimeoutTerminatesReconnectWithoutOverlappingConnection() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"async-close-timeout");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();

    (void)fixture.Service.Reconnect(L"async-close-timeout");
    fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
    Check(StateFor(fixture.Service, L"async-close-timeout") == DeviceLifecycleState::Failed,
          "a missing close callback must make the timed-out operation terminal while retaining its barrier");

    auto reconnect = fixture.Service.Reconnect(L"async-close-timeout");
    Check(fixture.Service.WaitForCompletion(reconnect) == DeviceOperationStatus::Failed,
          "a reconnect after close timeout must retain the terminal failure until close is confirmed");
    Check(fixture.ConnectionAccess->Connections.size() == createCount && oldConnection->CloseCalls == 1,
          "the terminal outcome must not weaken the close-before-reconnect barrier");
}

void TestCloseBarrierTimerSetupFailureTerminatesReconnectWithoutOverlappingConnection() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"async-close-timer-failure");
    auto* const oldConnection = fixture.ConnectionAccess->LastConnection;
    auto const createCount = fixture.ConnectionAccess->Connections.size();

    fixture.TimerAccess->ReturnNullNextSchedule = true;
    (void)fixture.Service.Reconnect(L"async-close-timer-failure");
    Check(StateFor(fixture.Service, L"async-close-timer-failure") == DeviceLifecycleState::Failed,
          "a missing close-barrier timer must make the reconnect operation terminal");

    auto reconnect = fixture.Service.Reconnect(L"async-close-timer-failure");
    Check(fixture.Service.WaitForCompletion(reconnect) == DeviceOperationStatus::Failed,
          "a reconnect after close timer failure must retain the terminal failure until close is confirmed");
    Check(fixture.ConnectionAccess->Connections.size() == createCount && oldConnection->CloseCalls == 1,
          "a terminal timer setup failure must retain the close barrier without creating a replacement");

    oldConnection->CompleteClose();
    CompleteCloseAndCooldown(fixture, oldConnection);
    Check(StateFor(fixture.Service, L"async-close-timer-failure") == DeviceLifecycleState::Idle,
          "a late close completion must settle the retained barrier after timer setup failure");
}

void TestResumeWatcherFailureClearsRunningStateAndAllowsRetry() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the resume failure fixture");
    auto* const stoppedWatcher = fixture.WatcherAccess->LastWatcher;

    fixture.Service.Suspend();
    fixture.WatcherAccess->FailNextStart = true;
    fixture.Service.Resume();
    auto* const failedResumeWatcher = fixture.WatcherAccess->LastWatcher;
    auto const failedSnapshot = fixture.Service.Snapshot();
    Check(failedResumeWatcher && failedResumeWatcher != stoppedWatcher && failedResumeWatcher->StartCalls == 1,
          "resume must attempt a fresh watcher generation");
    Check(!failedSnapshot.IsRunning && !failedSnapshot.IsSuspended,
          "a failed watcher restart must leave the service resumed but not running");

    failedResumeWatcher->Add(L"stale-resume", L"Stale resume");
    Check(fixture.Service.Snapshot().Inventory.Devices.empty(),
          "callbacks from the failed resume generation must remain rejected");

    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "a failed watcher restart must permit a later Start retry");
    auto* const retriedWatcher = fixture.WatcherAccess->LastWatcher;
    Check(retriedWatcher && retriedWatcher != failedResumeWatcher,
          "the Start retry must use a newer watcher generation");
    if (!retriedWatcher) return;
    failedResumeWatcher->Add(L"late-resume", L"Late resume");
    retriedWatcher->Add(L"fresh-resume", L"Fresh resume");
    auto const retriedSnapshot = fixture.Service.Snapshot();
    Check(retriedSnapshot.IsRunning && retriedSnapshot.Inventory.Devices.size() == 1 &&
              retriedSnapshot.Inventory.Devices.front().Id == L"fresh-resume",
          "the successful retry must accept only the current watcher generation");
}

void TestIncomingCallbackOrderingAndLossFollowReconnectPolicy() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"incoming", L"Incoming");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    connection->Signal(DeviceConnectionState::Opened);
    connection->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Connected,
          "a delayed incoming Start completion must not overwrite an established Connected state");
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();
    connection->Signal(DeviceConnectionState::Closed);
    auto* const retryTimer = fixture.TimerAccess->LastTimer;
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount &&
              StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::WaitingForReconnect &&
              SessionFor(fixture.Service, L"incoming").HasConnection &&
              fixture.ConnectionAccess->LastConnection == connection,
          "an established incoming loss must retain its listener while it waits for the configured reconnect policy");
    auto const waitingEpoch = SessionFor(fixture.Service, L"incoming").OperationEpoch;
    auto const factCount = fixture.Facts.size();
    connection->Signal(DeviceConnectionState::Closed);
    Check(
        StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::WaitingForReconnect &&
            SessionFor(fixture.Service, L"incoming").HasConnection &&
            SessionFor(fixture.Service, L"incoming").OperationEpoch == waitingEpoch &&
            fixture.ConnectionAccess->LastConnection == connection && fixture.TimerAccess->LastTimer == retryTimer &&
            fixture.Facts.size() == factCount,
        "a duplicate incoming closed callback while waiting for reconnect must not mutate the retained listener state");
    retryTimer->FireEvenIfCancelled();
    Check(connection->CloseCalls == 1 && fixture.ConnectionAccess->Connections.size() == connectionCount &&
              StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Disconnecting,
          "the incoming reconnect timer must close the retained listener through the close barrier before replacement");
    CompleteCloseAndCooldown(fixture, connection);
    Check(
        fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
        "the incoming reconnect timer must recreate a listener only after the retained listener close barrier settles");
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Idle,
          "an incoming reconnect must return to listening state without OpenAsync");

    auto* const retainedListener = fixture.ConnectionAccess->LastConnection;
    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Opened);
    fixture.Service.ConfigureReconnectPolicy(false, {});
    auto const failureCount = std::ranges::count_if(
        fixture.Facts, [](DeviceFact const& fact) { return fact.DeviceId == L"incoming" && fact.IsTerminalFailure; });
    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Idle &&
              SessionFor(fixture.Service, L"incoming").HasConnection &&
              fixture.ConnectionAccess->LastConnection == retainedListener,
          "an established incoming loss with reconnect disabled must retain the ready listener instead of failing");
    Check(std::ranges::count_if(fixture.Facts,
                                [](DeviceFact const& fact) {
                                    return fact.DeviceId == L"incoming" && fact.IsTerminalFailure;
                                }) == failureCount,
          "a retained incoming listener loss must not publish a terminal failure fact when reconnect is disabled");
    retainedListener->Signal(DeviceConnectionState::Opened);
    Check(StateFor(fixture.Service, L"incoming") == DeviceLifecycleState::Connected,
          "the retained incoming listener must accept a later opened callback");
}

void TestDisablingReconnectRestoresPendingIncomingListenerAfterCloseBarrier() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the pending incoming policy fixture");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"incoming-policy-disable", L"Incoming policy disable");
    auto* const listener = fixture.ConnectionAccess->LastConnection;
    listener->Signal(DeviceConnectionState::Opened);
    listener->Signal(DeviceConnectionState::Closed);
    auto* const retryTimer = fixture.TimerAccess->LastTimer;
    auto const retryCallback = retryTimer->Callback;
    auto const retryTimerCancellationState = retryTimer->CancellationState;
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();
    auto const terminalFailureCount = std::ranges::count_if(fixture.Facts, [](DeviceFact const& fact) {
        return fact.DeviceId == L"incoming-policy-disable" && fact.IsTerminalFailure;
    });

    fixture.Service.ConfigureReconnectPolicy(false, {});
    Check(*retryTimerCancellationState, "disabling reconnect policy must cancel the pending incoming retry timer");
    Check(listener->CloseCalls == 1, "disabling reconnect policy must close the retained incoming listener");
    if (retryCallback) retryCallback();
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount,
          "a cancelled incoming retry timer must not bypass the close barrier");

    listener->CompleteClose();
    auto* const cooldown = fixture.TimerAccess->LastTimer;
    Check(cooldown && cooldown->Delay == std::chrono::milliseconds(1500) &&
              fixture.ConnectionAccess->Connections.size() == connectionCount,
          "policy cancellation must retain the incoming listener close cooldown before recreation");
    if (!cooldown) return;
    cooldown->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
          "the closed incoming listener must be recreated only after the cooldown barrier");

    auto* const restoredListener = fixture.ConnectionAccess->LastConnection;
    restoredListener->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming-policy-disable") == DeviceLifecycleState::Idle &&
              SessionFor(fixture.Service, L"incoming-policy-disable").HasConnection &&
              !restoredListener->OpenCompletion &&
              std::ranges::count_if(fixture.Facts,
                                    [](DeviceFact const& fact) {
                                        return fact.DeviceId == L"incoming-policy-disable" && fact.IsTerminalFailure;
                                    }) == terminalFailureCount,
          "policy cancellation must restore a non-opening incoming listener without a terminal failure");
}

void TestExplicitDisconnectRestoresIncomingListenerAfterOutgoingRetry() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the outgoing retry fixture");
    fixture.WatcherAccess->LastWatcher->Add(L"outgoing-retry", L"Outgoing retry");
    ConnectSuccessfully(fixture, L"outgoing-retry");
    auto* const outgoingConnection = fixture.ConnectionAccess->LastConnection;

    fixture.Service.ConfigureIncomingConnections(true);
    outgoingConnection->Signal(DeviceConnectionState::Closed);
    auto* const retryTimer = fixture.TimerAccess->LastTimer;
    auto const staleRetryCallback = retryTimer->Callback;
    auto const retryTimerCancellationState = retryTimer->CancellationState;
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();
    Check(StateFor(fixture.Service, L"outgoing-retry") == DeviceLifecycleState::WaitingForReconnect &&
              !SessionFor(fixture.Service, L"outgoing-retry").HasConnection,
          "an outgoing loss must reach waiting-reconnect without retaining a current connection");

    Check(fixture.Service.Disconnect(L"outgoing-retry").Kind == DeviceCommandResultKind::Accepted,
          "an explicit disconnect must cancel the pending outgoing retry");
    auto* const incomingListener = fixture.ConnectionAccess->LastConnection;
    Check(*retryTimerCancellationState && fixture.ConnectionAccess->Connections.size() == connectionCount + 1 &&
              incomingListener != outgoingConnection &&
              SessionFor(fixture.Service, L"outgoing-retry").IsReconnectCancelled,
          "disconnecting a waiting outgoing session must retain cancellation while creating the incoming listener");

    if (staleRetryCallback) staleRetryCallback();
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
          "a stale outgoing retry must not replace the restored incoming listener");

    incomingListener->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"outgoing-retry") == DeviceLifecycleState::Idle &&
              SessionFor(fixture.Service, L"outgoing-retry").HasConnection && !incomingListener->OpenCompletion,
          "the restored incoming listener must be active without an outgoing open request");
    incomingListener->Signal(DeviceConnectionState::Opened);
    Check(StateFor(fixture.Service, L"outgoing-retry") == DeviceLifecycleState::Connected,
          "the restored incoming listener must accept a simulated incoming connection");
}

void TestDisablingIncomingClosesEstablishedAndPendingIncomingSessions() {
    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the incoming-disable fixture");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.WatcherAccess->LastWatcher->Add(L"incoming-disable", L"Incoming disable");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->Signal(DeviceConnectionState::Opened);
        fixture.Service.ConfigureIncomingConnections(false);
        Check(connection->CloseCalls == 1,
              "disabling incoming connections must close an established incoming listener exactly once");
        CompleteCloseAndCooldown(fixture, connection);
        Check(StateFor(fixture.Service, L"incoming-disable") == DeviceLifecycleState::Idle &&
                  !SessionFor(fixture.Service, L"incoming-disable").HasConnection,
              "disabling incoming connections must settle an established listener without leaving it connected");
    }

    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the incoming-pending fixture");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.WatcherAccess->LastWatcher->Add(L"incoming-pending", L"Incoming pending");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->Signal(DeviceConnectionState::Opened);
        connection->Signal(DeviceConnectionState::Closed);
        auto* const pendingRetry = fixture.TimerAccess->LastTimer;
        auto const pendingRetryCallback = pendingRetry->Callback;
        fixture.Service.ConfigureIncomingConnections(false);
        Check(connection->CloseCalls == 1 &&
                  StateFor(fixture.Service, L"incoming-pending") == DeviceLifecycleState::Disconnecting,
              "disabling incoming connections must cancel pending retry and close the retained incoming listener");
        connection->CompleteClose();
        auto* const closeCooldown = fixture.TimerAccess->LastTimer;
        Check(closeCooldown && closeCooldown->Delay == std::chrono::milliseconds(1500),
              "disabling incoming connections must retain the close barrier until completion");
        if (!closeCooldown) return;
        closeCooldown->FireEvenIfCancelled();
        Check(StateFor(fixture.Service, L"incoming-pending") == DeviceLifecycleState::Idle &&
                  !SessionFor(fixture.Service, L"incoming-pending").HasConnection,
              "disabling incoming connections must settle the retained listener after the close barrier");
        if (pendingRetryCallback) pendingRetryCallback();
        Check(fixture.ConnectionAccess->Connections.size() == 1,
              "a stale pending incoming reconnect timer must not recreate a listener after incoming is disabled");
    }
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

void TestRemovingAnIdleIncomingListenerClosesWithoutTerminalFailure() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the incoming-removal fixture");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"incoming-removed", L"Incoming removed");
    auto* const listener = fixture.ConnectionAccess->LastConnection;
    listener->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"incoming-removed") == DeviceLifecycleState::Idle &&
              SessionFor(fixture.Service, L"incoming-removed").HasConnection,
          "an incoming listener must be ready and idle before removal cleanup");
    fixture.Facts.clear();

    fixture.WatcherAccess->LastWatcher->Remove(L"incoming-removed");
    Check(listener->CloseCalls == 1 &&
              StateFor(fixture.Service, L"incoming-removed") == DeviceLifecycleState::Disconnecting,
          "removing an idle incoming listener must close it through the serialized close barrier");
    CompleteCloseAndCooldown(fixture, listener);

    Check(StateFor(fixture.Service, L"incoming-removed") == DeviceLifecycleState::Idle &&
              !SessionFor(fixture.Service, L"incoming-removed").HasConnection,
          "removing an idle incoming listener must settle to idle instead of failed");
    Check(!std::ranges::any_of(
              fixture.Facts,
              [](DeviceFact const& fact) { return fact.DeviceId == L"incoming-removed" && fact.IsTerminalFailure; }),
          "removing an idle incoming listener must not publish a terminal failure fact");
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

void TestManualTransientOpenRetriesPreserveFailureClassificationAndCancellation() {
    {
        Fixture fixture;
        (void)fixture.Service.Connect(L"transient-open-success");
        auto* const first = fixture.ConnectionAccess->LastConnection;
        first->CompleteStart(DeviceConnectionResult::Success);
        first->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        Check(first->CloseCalls == 1, "a transient Open failure must close before scheduling its retry");
        CompleteCloseAndCooldown(fixture, first);
        auto* const retryTimer = fixture.TimerAccess->LastTimer;
        Check(retryTimer->Delay == std::chrono::milliseconds::zero(),
              "the first transient Open retry must use the legacy maximum with the completed close cooldown");
        retryTimer->FireEvenIfCancelled();
        auto* const second = fixture.ConnectionAccess->LastConnection;
        second->CompleteStart(DeviceConnectionResult::Success);
        second->CompleteOpen(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"transient-open-success") == DeviceLifecycleState::Connected,
              "a successful transient Open retry must establish the original manual operation");
    }

    {
        Fixture fixture;
        (void)fixture.Service.Connect(L"qualified-unknown-open");
        auto* const first = fixture.ConnectionAccess->LastConnection;
        first->CompleteStart(DeviceConnectionResult::Success);
        first->CompleteOpen({.Result = DeviceConnectionResult::Failed, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, first);
        Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::milliseconds::zero(),
              "a qualified UnknownFailure must retain transient classification and legacy maximum timing");
    }

    {
        Fixture fixture;
        (void)fixture.Service.Connect(L"permanent-open-failure");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen(DeviceConnectionResult::Failed);
        CompleteCloseAndCooldown(fixture, connection);
        Check(StateFor(fixture.Service, L"permanent-open-failure") == DeviceLifecycleState::Failed &&
                  fixture.ConnectionAccess->Connections.size() == 1,
              "an unqualified Open failure must remain terminal instead of entering transient retry");
    }

    {
        Fixture fixture;
        (void)fixture.Service.Connect(L"cancel-transient-open");
        auto* const first = fixture.ConnectionAccess->LastConnection;
        first->CompleteStart(DeviceConnectionResult::Success);
        first->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, first);
        auto const staleRetryCallback = fixture.TimerAccess->LastTimer->Callback;
        (void)fixture.Service.CancelReconnect(L"cancel-transient-open");
        if (staleRetryCallback) staleRetryCallback();
        Check(StateFor(fixture.Service, L"cancel-transient-open") == DeviceLifecycleState::Idle &&
                  fixture.ConnectionAccess->Connections.size() == 1,
              "manual cancellation must invalidate a transient Open retry timer without creating a new connection");
    }
}

void TestManualTransientOpenRetryExhaustsAtTheCharacterizedLimit() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the transient exhaustion incoming fixture");
    fixture.Service.ConfigureIncomingConnections(true);
    fixture.WatcherAccess->LastWatcher->Add(L"transient-open-exhaust", L"Transient open exhaust");
    auto* const originalListener = fixture.ConnectionAccess->LastConnection;
    originalListener->CompleteStart(DeviceConnectionResult::Success);
    (void)fixture.Service.Connect(L"transient-open-exhaust");
    CompleteCloseAndCooldown(fixture, originalListener);

    for (std::size_t failedAttempt = 1; failedAttempt <= 10; ++failedAttempt) {
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, connection);
        if (failedAttempt == 10) break;
        auto* const retryTimer = fixture.TimerAccess->LastTimer;
        Check(retryTimer->Delay == std::chrono::milliseconds(std::array<int, 9>{
                                       0, 0, 0, 1000, 2500, 4500, 6500, 6500, 6500}[failedAttempt - 1]),
              "each transient Open retry must retain the characterized maximum including the close cooldown");
        retryTimer->FireEvenIfCancelled();
    }

    auto* const restoredListener = fixture.ConnectionAccess->LastConnection;
    restoredListener->CompleteStart(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"transient-open-exhaust") == DeviceLifecycleState::Idle &&
              fixture.ConnectionAccess->Connections.size() == 12 && !restoredListener->OpenCompletion,
          "transient Open exhaustion must restore the incoming listener without an eleventh outgoing attempt");
    Check(std::ranges::any_of(fixture.Facts,
                              [](DeviceFact const& fact) {
                                  return fact.DeviceId == L"transient-open-exhaust" && fact.IsTerminalFailure &&
                                         fact.ConnectionResult == DeviceConnectionResult::TimedOut;
                              }),
          "transient Open exhaustion must retain the terminal timeout classification");
}

void TestAutomaticTransientOpenRetriesUseTheSameBoundedPolicy() {
    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"automatic-transient-open-success");
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
        auto* const firstAutomaticConnection = fixture.ConnectionAccess->LastConnection;
        firstAutomaticConnection->CompleteStart(DeviceConnectionResult::Success);
        firstAutomaticConnection->CompleteOpen(
            {.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, firstAutomaticConnection);
        auto* const transientRetryTimer = fixture.TimerAccess->LastTimer;
        Check(transientRetryTimer->Delay == std::chrono::milliseconds::zero() &&
                  SessionFor(fixture.Service, L"automatic-transient-open-success").CompletedRetryAttempts == 0,
              "an automatic transient Open failure must use the legacy maximum without consuming another reconnect "
              "attempt");
        transientRetryTimer->FireEvenIfCancelled();
        auto* const recoveredConnection = fixture.ConnectionAccess->LastConnection;
        recoveredConnection->CompleteStart(DeviceConnectionResult::Success);
        recoveredConnection->CompleteOpen(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"automatic-transient-open-success") == DeviceLifecycleState::Connected,
              "an automatic transient Open retry must recover the same automatic operation");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"automatic-transient-open-exhaust");
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        fixture.TimerAccess->LastTimer->FireEvenIfCancelled();

        for (std::size_t failedAttempt = 1; failedAttempt <= 10; ++failedAttempt) {
            auto* const connection = fixture.ConnectionAccess->LastConnection;
            connection->CompleteStart(DeviceConnectionResult::Success);
            connection->CompleteOpen({.Result = DeviceConnectionResult::Failed, .IsTransientFailure = true});
            CompleteCloseAndCooldown(fixture, connection);
            if (failedAttempt == 10) break;
            fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
        }

        Check(StateFor(fixture.Service, L"automatic-transient-open-exhaust") == DeviceLifecycleState::Failed &&
                  fixture.ConnectionAccess->Connections.size() == 11,
              "the tenth automatic transient Open failure must terminate without scheduling an eleventh inner retry");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"automatic-transient-open-cancel");
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        fixture.TimerAccess->LastTimer->FireEvenIfCancelled();
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, connection);
        auto const staleRetryCallback = fixture.TimerAccess->LastTimer->Callback;
        (void)fixture.Service.CancelReconnect(L"automatic-transient-open-cancel");
        if (staleRetryCallback) staleRetryCallback();
        Check(StateFor(fixture.Service, L"automatic-transient-open-cancel") == DeviceLifecycleState::Idle &&
                  fixture.ConnectionAccess->Connections.size() == 2,
              "cancellation must invalidate an automatic transient Open retry callback");
    }
}

void TestAutomaticPreEstablishmentCloseCountsEachAttemptOnce() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"automatic-pre-establishment-close");
    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);

    for (std::size_t attempt = 1; attempt <= 10; ++attempt) {
        auto* const retryTimer = fixture.TimerAccess->LastTimer;
        retryTimer->FireEvenIfCancelled();
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const staleClosedCallback = connection->StateChanged;
        connection->Signal(DeviceConnectionState::Closed);
        if (attempt == 1 && staleClosedCallback) staleClosedCallback(DeviceConnectionState::Closed);
        Check(SessionFor(fixture.Service, L"automatic-pre-establishment-close").CompletedRetryAttempts == attempt,
              "each automatic close-before-connected callback must consume exactly one retry attempt");
    }

    Check(StateFor(fixture.Service, L"automatic-pre-establishment-close") == DeviceLifecycleState::Failed &&
              SessionFor(fixture.Service, L"automatic-pre-establishment-close").CompletedRetryAttempts == 10,
          "repeated automatic pre-establishment closes must reach bounded retry exhaustion exactly once per attempt");
}

void TestPreEstablishmentClosePublishesOperationFailure() {
    Fixture fixture;
    (void)fixture.Service.Connect(L"manual-pre-establishment-close");
    fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);

    Check(std::ranges::any_of(fixture.Facts,
                              [](DeviceFact const& fact) {
                                  return fact.DeviceId == L"manual-pre-establishment-close" &&
                                         fact.Kind == DeviceFactKind::OperationFailed && fact.IsTerminalFailure &&
                                         fact.DisconnectReason == DeviceDisconnectReason::None;
                              }),
          "a manual close before establishment must be an operation failure without an unexpected-loss fact");
    Check(!std::ranges::any_of(fixture.Facts,
                               [](DeviceFact const& fact) {
                                   return fact.DeviceId == L"manual-pre-establishment-close" &&
                                          fact.DisconnectReason == DeviceDisconnectReason::UnexpectedLoss;
                               }),
          "UnexpectedLoss must remain reserved for an established connection loss");
}

void TestTerminalOutgoingPathsRestoreIncomingListener() {
    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the outgoing failure listener fixture");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.WatcherAccess->LastWatcher->Add(L"restore-after-start-failure", L"Restore after start failure");
        auto* const listener = fixture.ConnectionAccess->LastConnection;
        listener->CompleteStart(DeviceConnectionResult::Success);
        fixture.ConnectionAccess->NextBehavior.ThrowOnStart = true;
        (void)fixture.Service.Connect(L"restore-after-start-failure");
        CompleteCloseAndCooldown(fixture, listener);
        auto* const failedOutgoing = fixture.ConnectionAccess->LastConnection;
        fixture.ConnectionAccess->NextBehavior = {};
        CompleteCloseAndCooldown(fixture, failedOutgoing);
        auto* const restoredListener = fixture.ConnectionAccess->LastConnection;
        restoredListener->CompleteStart(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"restore-after-start-failure") == DeviceLifecycleState::Idle &&
                  restoredListener != failedOutgoing && !restoredListener->OpenCompletion,
              "a terminal outgoing Start failure must restore the incoming listener after its close barrier");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"restore-after-unexpected-loss");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.Service.ConfigureReconnectPolicy(false, {});
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        auto* const restoredListener = fixture.ConnectionAccess->LastConnection;
        restoredListener->CompleteStart(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"restore-after-unexpected-loss") == DeviceLifecycleState::Idle &&
                  SessionFor(fixture.Service, L"restore-after-unexpected-loss").HasConnection &&
                  !restoredListener->OpenCompletion,
              "terminal established unexpected loss must restore an enabled incoming listener");
    }

    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the outgoing cancellation listener fixture");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.WatcherAccess->LastWatcher->Add(L"restore-after-cancellation", L"Restore after cancellation");
        auto* const listener = fixture.ConnectionAccess->LastConnection;
        listener->CompleteStart(DeviceConnectionResult::Success);

        auto operation = fixture.Service.Connect(L"restore-after-cancellation");
        CompleteCloseAndCooldown(fixture, listener);
        auto* const outgoing = fixture.ConnectionAccess->LastConnection;
        std::stop_source cancellation;
        cancellation.request_stop();
        Check(fixture.Service.WaitForCompletion(operation, cancellation.get_token()) ==
                  DeviceOperationStatus::Cancelled,
              "cancelling an accepted operation must report cancellation");
        Check(outgoing->CloseCalls == 1,
              "cancelling a replacement outgoing connection must close it before restoring the listener");
        CompleteCloseAndCooldown(fixture, outgoing);
        auto* const restoredListener = fixture.ConnectionAccess->LastConnection;
        restoredListener->CompleteStart(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"restore-after-cancellation") == DeviceLifecycleState::Idle &&
                  SessionFor(fixture.Service, L"restore-after-cancellation").IsReconnectCancelled &&
                  !restoredListener->OpenCompletion,
              "outgoing cancellation must restore the incoming listener without clearing user cancellation");
    }
}

void TestConnectAndReconnectRejectCloseBarrierOverlap() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"async-close-barrier-overlap");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    (void)fixture.Service.Reconnect(L"async-close-barrier-overlap");

    auto connect = fixture.Service.Connect(L"async-close-barrier-overlap");
    auto reconnect = fixture.Service.Reconnect(L"async-close-barrier-overlap");
    Check(fixture.Service.WaitForCompletion(connect) == DeviceOperationStatus::Rejected &&
              fixture.Service.WaitForCompletion(reconnect) == DeviceOperationStatus::Rejected,
          "overlapping commands must return rejection without waiting for another command's close barrier");

    CompleteCloseAndCooldown(fixture, connection);
    Check(StateFor(fixture.Service, L"async-close-barrier-overlap") == DeviceLifecycleState::Connecting,
          "the original reconnect must retain ownership after overlapping commands are rejected");
}

void TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"retry");
    auto* const first = fixture.ConnectionAccess->LastConnection;
    first->Signal(DeviceConnectionState::Closed);
    auto* timer = fixture.TimerAccess->LastTimer;
    Check(StateFor(fixture.Service, L"retry") == DeviceLifecycleState::WaitingForReconnect,
          "unexpected loss must schedule the bounded reconnect policy");
    Check(timer->Delay == std::chrono::seconds(5) && SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 0,
          "the initial reconnect timer must be attempt one at five seconds without consuming a completed failure");
    auto firstTimerCallback = timer->Callback;
    if (firstTimerCallback) firstTimerCallback();
    auto* const retry = fixture.ConnectionAccess->LastConnection;
    retry->CompleteStart(DeviceConnectionResult::Success);
    retry->CompleteOpen(DeviceConnectionResult::Failed);
    CompleteCloseAndCooldown(fixture, retry);
    Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::seconds(10) &&
              SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 1,
          "only a completed automatic attempt may advance the retry count and backoff");
    timer = fixture.TimerAccess->LastTimer;
    auto staleTimerCallback = timer->Callback;
    (void)fixture.Service.CancelReconnect(L"retry");
    if (staleTimerCallback) staleTimerCallback();
    Check(fixture.ConnectionAccess->Connections.size() == 2 &&
              SessionFor(fixture.Service, L"retry").CompletedRetryAttempts == 1,
          "a cancelled pending timer must not create or count another automatic attempt");
}

void TestManualCommandsCancelSupersededReconnectEpochs() {
    for (auto const& [deviceId, command] : std::vector<std::pair<std::wstring, DeviceCommandKind>>{
             {L"cancel-manual-connect", DeviceCommandKind::Connect},
             {L"cancel-manual-reconnect", DeviceCommandKind::Reconnect},
         }) {
        Fixture fixture;
        ConnectSuccessfully(fixture, deviceId);
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        auto const waitingEpoch = SessionFor(fixture.Service, deviceId).OperationEpoch;
        Check(StateFor(fixture.Service, deviceId) == DeviceLifecycleState::WaitingForReconnect,
              "a lost connection must wait before the manual supersession test");

        auto operation = command == DeviceCommandKind::Connect ? fixture.Service.Connect(deviceId)
                                                               : fixture.Service.Reconnect(deviceId);
        auto* const manualConnection = fixture.ConnectionAccess->LastConnection;
        Check(SessionFor(fixture.Service, deviceId).OperationEpoch > waitingEpoch &&
                  StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Connecting,
              "a manual command from WaitingForReconnect must own a new operation epoch");

        std::stop_source cancellation;
        cancellation.request_stop();
        Check(fixture.Service.WaitForCompletion(operation, cancellation.get_token()) ==
                  DeviceOperationStatus::Cancelled,
              "cancelling an accepted operation must report cancellation");
        Check(manualConnection->CloseCalls == 1 &&
                  StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Disconnecting,
              "cancelling a superseding manual command must close its exact operation");

        manualConnection->CompleteStart(DeviceConnectionResult::Success);
        manualConnection->CompleteOpen(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Disconnecting,
              "late start and open completions after cancellation must not reconnect the session");
        CompleteCloseAndCooldown(fixture, manualConnection);
        Check(StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Idle,
              "the cancelled manual operation must settle without a replacement connection");
        Check(fixture.Service.WaitForCompletion(operation) == DeviceOperationStatus::Cancelled,
              "a cancelled operation must settle before its fixture is destroyed");
    }
}

void TestReconnectPolicyAndUserCancellationRemainDistinct() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"policy");

    fixture.Service.ConfigureReconnectPolicy(false, {});
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "disabling reconnect policy must be observable independently of user cancellation");
    fixture.Service.ConfigureReconnectPolicy(true, {L"policy"});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "re-enabling reconnect policy must allow later connection-loss retries");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not be recorded as user cancellation");

    (void)fixture.Service.CancelReconnect(L"policy");
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "manual reconnect cancellation must remain observable as user state");
    fixture.Service.ConfigureReconnectPolicy(false, {});
    fixture.Service.ConfigureReconnectPolicy(true, {L"policy"});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not erase an explicit user cancellation");

    auto* const connection = fixture.ConnectionAccess->LastConnection;
    (void)fixture.Service.Disconnect(L"policy");
    CompleteCloseAndCooldown(fixture, connection);
    (void)fixture.Service.Connect(L"policy");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "a later manual connect from an idle session must explicitly clear user cancellation");
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

void TestBulkSuspendResumeAndShutdownCannotResurrectSessions() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"a");
    ConnectSuccessfully(fixture, L"b");
    (void)fixture.Service.DisconnectAll();
    for (auto* connection : fixture.ConnectionAccess->Connections)
        CompleteCloseAndCooldown(fixture, connection);
    Check(StateFor(fixture.Service, L"a") == DeviceLifecycleState::Idle &&
              StateFor(fixture.Service, L"b") == DeviceLifecycleState::Idle,
          "bulk disconnect must settle every serialized session");

    ConnectSuccessfully(fixture, L"resume");
    auto* const busy = fixture.ConnectionAccess->LastConnection;
    auto const beforeResume = fixture.ConnectionAccess->Connections.size();
    fixture.Service.Suspend();
    fixture.Service.Resume();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume,
          "resume must not create a replacement before the suspend close barrier completes");
    busy->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume,
          "a suspend close must retain the barrier during the required cooldown");
    auto* const suspendCooldown = fixture.TimerAccess->LastTimer;
    Check(suspendCooldown->Delay == std::chrono::milliseconds(1500),
          "a suspend close must use the retained close cooldown");
    suspendCooldown->FireEvenIfCancelled();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
          "the current suspend close completion must release the resume replacement");
    busy->CompleteClose();
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
          "a stale suspend close completion must not create another replacement");

    auto* const late = fixture.ConnectionAccess->LastConnection;
    fixture.Service.Shutdown();
    late->CompleteStart(DeviceConnectionResult::Success);
    late->Signal(DeviceConnectionState::Opened);
    Check(fixture.Service.Snapshot().IsShutdown && fixture.Service.Snapshot().Sessions.empty(),
          "shutdown must release sessions and reject every late platform callback");
}

void TestStartupPolicyAndDelayedPowerResume() {
    {
        Fixture fixture;
        fixture.Service.ConnectStartupTargets({L"startup"});
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen(DeviceConnectionResult::Success);
        connection->Signal(DeviceConnectionState::Closed);
        Check(StateFor(fixture.Service, L"startup") == DeviceLifecycleState::WaitingForReconnect &&
                  fixture.TimerAccess->LastTimer->Delay == std::chrono::seconds(5),
              "startup connections must retain the bounded loss-reconnect policy");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"power");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const beforeResume = fixture.ConnectionAccess->Connections.size();
        fixture.Service.Suspend();
        fixture.Service.ResumeAfterPowerTransition();
        fixture.Service.ResumeSuspendedSessions({L"power"});
        Check(fixture.ConnectionAccess->Connections.size() == beforeResume,
              "power resume must keep session reconnection delayed until the coordinator delivery");
        CompleteCloseAndCooldown(fixture, connection);
        Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
              "the delayed power-resume delivery must release the suspended session");
    }
}

void TestIdleDiscoveryResumesForManualConnectAfterPowerTransition() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish an idle discovered session");
    fixture.WatcherAccess->LastWatcher->Add(L"idle-power", L"Idle power");
    Check(StateFor(fixture.Service, L"idle-power") == DeviceLifecycleState::Idle,
          "a discovered device without incoming connections must remain idle");

    fixture.Service.SuspendForPowerTransition();
    fixture.Service.ResumeAfterPowerTransition();
    Check(fixture.ConnectionAccess->Connections.empty(),
          "an idle discovery must not be automatically connected by the power transition");

    auto const connect = fixture.Service.Connect(L"idle-power");
    Check(connect.Kind == DeviceCommandResultKind::Accepted && fixture.ConnectionAccess->LastConnection != nullptr,
          "a manually requested connect after power resume must start a new connection");
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"idle-power") == DeviceLifecycleState::Connected,
          "a manually requested connect after power resume must reach connected");
}

void TestPowerTransitionRecoveryTargetsIncludeIncomingAndPendingReconnectWithoutConnectedSessions() {
    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the incoming power-recovery fixture");
        fixture.Service.ConfigureIncomingConnections(true);
        fixture.WatcherAccess->LastWatcher->Add(L"power-incoming", L"Power incoming");
        fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"power-incoming") == DeviceLifecycleState::Idle &&
                  fixture.Service.GetConnectedDevices().empty(),
              "an incoming-only listener must be idle while no device is connected");
        auto const targets = fixture.Service.GetPowerTransitionRecoveryDeviceIds();
        Check(std::ranges::find(targets, L"power-incoming") != targets.end(),
              "power recovery target capture must retain an incoming-only listener with zero connected devices");

        auto* const listener = fixture.ConnectionAccess->LastConnection;
        fixture.Service.SuspendForPowerTransition();
        fixture.Service.ResumeAfterPowerTransition();
        fixture.Service.ResumeSuspendedSessions(targets);
        CompleteCloseAndCooldown(fixture, listener);
        Check(fixture.ConnectionAccess->Connections.size() == 2,
              "the delayed recovery target must restart an incoming-only listener after its close barrier completes");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"power-pending");
        fixture.ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
        Check(StateFor(fixture.Service, L"power-pending") == DeviceLifecycleState::WaitingForReconnect &&
                  fixture.Service.GetConnectedDevices().empty(),
              "a pending reconnect must be recoverable while zero devices are connected");
        auto const targets = fixture.Service.GetPowerTransitionRecoveryDeviceIds();
        Check(std::ranges::find(targets, L"power-pending") != targets.end(),
              "power recovery target capture must retain a pending reconnect with zero connected devices");

        fixture.Service.SuspendForPowerTransition();
        fixture.Service.ResumeAfterPowerTransition();
        fixture.Service.ResumeSuspendedSessions(targets);
        Check(fixture.ConnectionAccess->Connections.size() == 2,
              "the delayed recovery target must resume a pending reconnect without waiting for its stale timer");
    }

    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
        fixture.WatcherAccess->LastWatcher->Add(L"power-idle", L"Power idle");
        auto const targets = fixture.Service.GetPowerTransitionRecoveryDeviceIds();
        Check(std::ranges::find(targets, L"power-idle") == targets.end(),
              "ordinary idle discovery must not become a power recovery target");
    }
}

void TestPowerTransitionDoesNotReleaseCloseInFlightBeforeDelayedResume() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"power-close");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    auto const beforeResume = fixture.ConnectionAccess->Connections.size();

    fixture.Service.SuspendForPowerTransition();
    fixture.Service.ResumeAfterPowerTransition();
    CompleteCloseAndCooldown(fixture, connection);
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume,
          "power transition must not release a close-in-flight session before delayed resume delivery");

    fixture.Service.ResumeSuspendedSessions({L"power-close"});
    Check(fixture.ConnectionAccess->Connections.size() == beforeResume + 1,
          "the delayed resume delivery must remain the only path that restarts a suspended close-in-flight session");
}

void TestManualCommandsDuringDelayedPowerRecoverySupersedeRecovery() {
    for (auto const& [deviceId, command] : std::vector<std::pair<std::wstring, DeviceCommandKind>>{
             {L"power-manual-connect", DeviceCommandKind::Connect},
             {L"power-manual-reconnect", DeviceCommandKind::Reconnect},
         }) {
        Fixture fixture;
        ConnectSuccessfully(fixture, deviceId);
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const connectionCount = fixture.ConnectionAccess->Connections.size();

        fixture.Service.SuspendForPowerTransition();
        fixture.Service.ResumeAfterPowerTransition();
        CompleteCloseAndCooldown(fixture, connection);
        Check(StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Idle,
              "the completed suspend close must leave the session available for manual recovery supersession");

        auto const result = command == DeviceCommandKind::Connect ? fixture.Service.Connect(deviceId)
                                                                  : fixture.Service.Reconnect(deviceId);
        Check(result.Command == command && result.Kind == DeviceCommandResultKind::Accepted,
              "a manual command during delayed power recovery must supersede the stale recovery intent");
        Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
              "a manual command must start immediately once the suspend close barrier has settled");

        fixture.Facts.clear();
        fixture.Service.ResumeSuspendedSessions({deviceId});
        Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1 &&
                  StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Connecting,
              "a stale delayed recovery delivery must not replace the superseding manual operation");
        fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
        fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Connected,
              "the superseding manual request must complete without delayed recovery resurrection");
    }
}

void TestTransientOpenRetryTimingUsesTheLegacyMaximum() {
    {
        Fixture fixture;
        fixture.Service.ConnectStartupTargets({L"startup-transient-timing"});
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, connection);
        Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::milliseconds::zero(),
              "startup transient Open retries must include the completed close cooldown in the legacy maximum");
    }

    {
        Fixture fixture;
        ConnectSuccessfully(fixture, L"resume-transient-timing");
        auto* const establishedConnection = fixture.ConnectionAccess->LastConnection;
        fixture.Service.Suspend();
        fixture.Service.Resume();
        CompleteCloseAndCooldown(fixture, establishedConnection);

        auto* const resumedConnection = fixture.ConnectionAccess->LastConnection;
        resumedConnection->CompleteStart(DeviceConnectionResult::Success);
        resumedConnection->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        CompleteCloseAndCooldown(fixture, resumedConnection);
        Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::milliseconds::zero(),
              "resume transient Open retries must include the completed close cooldown in the legacy maximum");
    }

    {
        Fixture fixture;
        (void)fixture.Service.Connect(L"transient-cooldown-fallback");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->CompleteStart(DeviceConnectionResult::Success);
        connection->CompleteOpen({.Result = DeviceConnectionResult::TimedOut, .IsTransientFailure = true});
        fixture.TimerAccess->ReturnNullNextSchedule = true;
        connection->CompleteClose();
        Check(fixture.TimerAccess->LastTimer->Delay == std::chrono::milliseconds(500),
              "a close cooldown scheduling fallback must retain the full transient retry delay");
    }
}

void TestManualCommandsSupersedeDelayedPowerRecoveryDuringCloseBarrier() {
    for (auto const& [deviceId, command] : std::vector<std::pair<std::wstring, DeviceCommandKind>>{
             {L"power-manual-connect-async", DeviceCommandKind::Connect},
             {L"power-manual-reconnect-async", DeviceCommandKind::Reconnect},
         }) {
        Fixture fixture;
        ConnectSuccessfully(fixture, deviceId);
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        auto const connectionCount = fixture.ConnectionAccess->Connections.size();

        fixture.Service.SuspendForPowerTransition();
        fixture.Service.ResumeAfterPowerTransition();
        auto operation = command == DeviceCommandKind::Connect ? fixture.Service.Connect(deviceId)
                                                               : fixture.Service.Reconnect(deviceId);
        Check(operation.Kind == DeviceCommandResultKind::Accepted &&
                  fixture.ConnectionAccess->Connections.size() == connectionCount,
              "a manual command must wait for, rather than overlap, a suspend close barrier");

        CompleteCloseAndCooldown(fixture, connection);
        Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
              "a manual command must start after superseding the suspend close barrier");
        fixture.Service.ResumeSuspendedSessions({deviceId});
        Check(fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
              "a stale delayed recovery callback must not resurrect an superseded session");

        fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
        fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
        Check(fixture.Service.WaitForCompletion(operation) == DeviceOperationStatus::Succeeded &&
                  StateFor(fixture.Service, deviceId) == DeviceLifecycleState::Connected,
              "a superseding manual command must complete at its terminal connected outcome");
    }
}

void TestPowerTransitionRecoveryCaptureRejectsStaleDelayedIntent() {
    Fixture fixture;
    ConnectSuccessfully(fixture, L"power-stale-intent");
    auto* const connection = fixture.ConnectionAccess->LastConnection;
    auto const connectionCount = fixture.ConnectionAccess->Connections.size();

    auto const recoveryTargets = fixture.Service.SuspendForPowerTransition();
    Check(std::ranges::find(recoveryTargets, L"power-stale-intent") != recoveryTargets.end(),
          "suspension must return the exact recovery intent captured with the serialized mutation");
    Check(fixture.Service.Disconnect(L"power-stale-intent").Kind == DeviceCommandResultKind::Rejected,
          "a disconnect arriving after atomic suspension must not interleave before the captured recovery intent");

    fixture.Service.ResumeAfterPowerTransition();
    CompleteCloseAndCooldown(fixture, connection);
    Check(StateFor(fixture.Service, L"power-stale-intent") == DeviceLifecycleState::Idle,
          "the suspended connection must wait for delayed recovery after its close barrier");

    Check(fixture.Service.Disconnect(L"power-stale-intent").Kind == DeviceCommandResultKind::Accepted,
          "a post-resume manual disconnect must advance the session operation epoch");
    fixture.Service.ResumeSuspendedSessions(recoveryTargets);
    Check(fixture.ConnectionAccess->Connections.size() == connectionCount &&
              StateFor(fixture.Service, L"power-stale-intent") == DeviceLifecycleState::Idle,
          "a delayed recovery delivery must reject an intent superseded by manual disconnect");

    Check(fixture.Service.Connect(L"power-stale-intent").Kind == DeviceCommandResultKind::Accepted &&
              fixture.ConnectionAccess->Connections.size() == connectionCount + 1,
          "rejecting stale recovery must release the local suspension for a later manual connection");
    fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture.ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(StateFor(fixture.Service, L"power-stale-intent") == DeviceLifecycleState::Connected,
          "the later manual connection must complete without resurrecting stale recovery work");
}

void TestUnmatchedPowerResumeRestartsWatcherWithoutResurrectingSessions() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the unmatched-resume fixture");
    auto* const staleWatcher = fixture.WatcherAccess->LastWatcher;

    fixture.Service.ResumeAfterPowerTransition();
    auto* const resumedWatcher = fixture.WatcherAccess->LastWatcher;
    Check(resumedWatcher && resumedWatcher != staleWatcher && resumedWatcher->StartCalls == 1 &&
              fixture.Service.Snapshot().IsRunning,
          "an unmatched resume must restore the intended watcher generation");

    staleWatcher->Add(L"stale-unmatched-resume", L"Stale unmatched resume");
    resumedWatcher->Add(L"fresh-unmatched-resume", L"Fresh unmatched resume");
    Check(fixture.Service.Snapshot().Inventory.Devices.size() == 1 &&
              fixture.Service.Snapshot().Inventory.Devices.front().Id == L"fresh-unmatched-resume" &&
              fixture.ConnectionAccess->Connections.empty(),
          "unmatched resume recovery must reject stale watcher callbacks without starting canceled sessions");
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
        fixture.Service.SetReconnectOnConnectionLoss(std::wstring(L"automatic-fact-operation"), true);
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
        fixture.Service.ConfigureReconnectPolicy(false, {});
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
    std::jthread publisher([&] { fixture.Service.ConfigureReconnectPolicy(false, {}); });
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

} // namespace

int RunDeviceServiceTests() {
    TestSettingsPolicyRejectsOldDuplicateAndCancelledRevisions();
    TestSettingsPolicyDoesNotWaitForForeignDeliveryAndCancelsQueuedWork();
    TestControllerOwnsDeviceFactNormalization();
    TestControllerStatusTokensFollowTheDeviceOwner();
    TestControllerDeviceFailureAndLifetime();
    TestCompletionRetainsFirstTerminalResult();
    TestCompletionCancellationOwnershipAndDeadline();
    TestCompletionWakesWaitersAndRejectsReentrantWait();
    TestCancellationDoesNotWaitBehindBlockedPublisher();
    TestOperationEpochRejectsStaleCompletion();
    TestReconnectWaitsForCloseAndRevokesTheOldToken();
    TestDuplicateConnectCoalescesWithoutReplacingConnectedSession();
    TestCloseBarrierTimeoutRetainsTheOldConnectionUntilLateCompletion();
    TestCloseBarrierTimeoutTerminatesReconnectWithoutOverlappingConnection();
    TestCloseBarrierTimerSetupFailureTerminatesReconnectWithoutOverlappingConnection();
    TestResumeWatcherFailureClearsRunningStateAndAllowsRetry();
    TestIncomingCallbackOrderingAndLossFollowReconnectPolicy();
    TestDisablingReconnectRestoresPendingIncomingListenerAfterCloseBarrier();
    TestExplicitDisconnectRestoresIncomingListenerAfterOutgoingRetry();
    TestDisablingIncomingClosesEstablishedAndPendingIncomingSessions();
    TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks();
    TestRemovingAnIdleDiscoveredDeviceDoesNotPublishTerminalFailure();
    TestRemovingAnIdleIncomingListenerClosesWithoutTerminalFailure();
    TestPlatformSetupExceptionsCleanUpAndPublishTerminalFacts();
    TestManualTransientOpenRetriesPreserveFailureClassificationAndCancellation();
    TestManualTransientOpenRetryExhaustsAtTheCharacterizedLimit();
    TestAutomaticTransientOpenRetriesUseTheSameBoundedPolicy();
    TestTransientOpenRetryTimingUsesTheLegacyMaximum();
    TestAutomaticPreEstablishmentCloseCountsEachAttemptOnce();
    TestPreEstablishmentClosePublishesOperationFailure();
    TestTerminalOutgoingPathsRestoreIncomingListener();
    TestConnectAndReconnectRejectCloseBarrierOverlap();
    TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks();
    TestManualCommandsCancelSupersededReconnectEpochs();
    TestReconnectPolicyAndUserCancellationRemainDistinct();
    TestConcurrentCommandWaitsForSerializedMutation();
    TestBulkSuspendResumeAndShutdownCannotResurrectSessions();
    TestStartupPolicyAndDelayedPowerResume();
    TestIdleDiscoveryResumesForManualConnectAfterPowerTransition();
    TestPowerTransitionRecoveryTargetsIncludeIncomingAndPendingReconnectWithoutConnectedSessions();
    TestPowerTransitionDoesNotReleaseCloseInFlightBeforeDelayedResume();
    TestManualCommandsDuringDelayedPowerRecoverySupersedeRecovery();
    TestManualCommandsSupersedeDelayedPowerRecoveryDuringCloseBarrier();
    TestPowerTransitionRecoveryCaptureRejectsStaleDelayedIntent();
    TestUnmatchedPowerResumeRestartsWatcherWithoutResurrectingSessions();
    TestStopAndShutdownReturnNormalizedTerminalResults();
    TestFactsCarryNormalizedSnapshots();
    TestFailureFactsRetainOperationKind();
    TestDisconnectReasonsSelectTheLockedNotificationPolicy();
    TestOperationFailuresRemainOperationFailuresWithoutDisconnectReason();
    return g_failures;
}
