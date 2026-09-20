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
/*//////// DeviceReconnect Tests /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

using namespace apc::tests::device;

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
    fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
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

    fixture.Service.ApplySettingsPolicy({1, false, false, {}, {}});
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "disabling reconnect policy must be observable independently of user cancellation");
    fixture.Service.ApplySettingsPolicy({2, false, true, {L"policy"}, {}});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectEnabled,
          "re-enabling reconnect policy must allow later connection-loss retries");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not be recorded as user cancellation");

    (void)fixture.Service.CancelReconnect(L"policy");
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "manual reconnect cancellation must remain observable as user state");
    fixture.Service.ApplySettingsPolicy({3, false, false, {}, {}});
    fixture.Service.ApplySettingsPolicy({4, false, true, {L"policy"}, {}});
    Check(SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "policy changes must not erase an explicit user cancellation");

    auto* const connection = fixture.ConnectionAccess->LastConnection;
    (void)fixture.Service.Disconnect(L"policy");
    CompleteCloseAndCooldown(fixture, connection);
    (void)fixture.Service.Connect(L"policy");
    Check(!SessionFor(fixture.Service, L"policy").IsReconnectCancelled,
          "a later manual connect from an idle session must explicitly clear user cancellation");
}

} // namespace

int RunDeviceReconnectTests() {
    TestReconnectWaitsForCloseAndRevokesTheOldToken();
    TestCloseBarrierTimeoutRetainsTheOldConnectionUntilLateCompletion();
    TestCloseBarrierTimeoutTerminatesReconnectWithoutOverlappingConnection();
    TestCloseBarrierTimerSetupFailureTerminatesReconnectWithoutOverlappingConnection();
    TestManualTransientOpenRetriesPreserveFailureClassificationAndCancellation();
    TestManualTransientOpenRetryExhaustsAtTheCharacterizedLimit();
    TestAutomaticTransientOpenRetriesUseTheSameBoundedPolicy();
    TestTransientOpenRetryTimingUsesTheLegacyMaximum();
    TestAutomaticPreEstablishmentCloseCountsEachAttemptOnce();
    TestPreEstablishmentClosePublishesOperationFailure();
    TestConnectAndReconnectRejectCloseBarrierOverlap();
    TestRetryTimerAndManualCancellationRejectStaleTimerCallbacks();
    TestManualCommandsCancelSupersededReconnectEpochs();
    TestReconnectPolicyAndUserCancellationRemainDistinct();
    return g_failures;
}
