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
/*//////// DeviceIncoming Tests //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

using namespace apc::tests::device;

void TestIncomingCallbackOrderingAndLossFollowReconnectPolicy() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted, "watcher start must be accepted");
    fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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
    fixture.Service.ApplySettingsPolicy({2, true, false, {}, {}});
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
    fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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

    fixture.Service.ApplySettingsPolicy({2, true, false, {}, {}});
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

    fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
        fixture.WatcherAccess->LastWatcher->Add(L"incoming-disable", L"Incoming disable");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->Signal(DeviceConnectionState::Opened);
        fixture.Service.ApplySettingsPolicy({2, false, true, {}, {}});
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
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
        fixture.WatcherAccess->LastWatcher->Add(L"incoming-pending", L"Incoming pending");
        auto* const connection = fixture.ConnectionAccess->LastConnection;
        connection->Signal(DeviceConnectionState::Opened);
        connection->Signal(DeviceConnectionState::Closed);
        auto* const pendingRetry = fixture.TimerAccess->LastTimer;
        auto const pendingRetryCallback = pendingRetry->Callback;
        fixture.Service.ApplySettingsPolicy({2, false, true, {}, {}});
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

void TestRemovingAnIdleIncomingListenerClosesWithoutTerminalFailure() {
    Fixture fixture;
    Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
          "watcher start must establish the incoming-removal fixture");
    fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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

void TestTerminalOutgoingPathsRestoreIncomingListener() {
    {
        Fixture fixture;
        Check(fixture.Service.Start().Kind == DeviceCommandResultKind::Accepted,
              "watcher start must establish the outgoing failure listener fixture");
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
        fixture.Service.ApplySettingsPolicy({2, true, false, {}, {}});
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
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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

} // namespace

int RunDeviceIncomingTests() {
    TestIncomingCallbackOrderingAndLossFollowReconnectPolicy();
    TestDisablingReconnectRestoresPendingIncomingListenerAfterCloseBarrier();
    TestExplicitDisconnectRestoresIncomingListenerAfterOutgoingRetry();
    TestDisablingIncomingClosesEstablishedAndPendingIncomingSessions();
    TestRemovingAnIdleIncomingListenerClosesWithoutTerminalFailure();
    TestTerminalOutgoingPathsRestoreIncomingListener();
    return g_failures;
}
