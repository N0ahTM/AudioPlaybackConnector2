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
/*//////// DevicePower Tests /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {

using namespace apc::tests::device;

bool HasConnectedSession(DeviceService& service) {
    auto const snapshot = service.Snapshot();
    return std::ranges::any_of(snapshot.Sessions,
                               [](auto const& session) { return session.State == DeviceLifecycleState::Connected; });
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
        fixture.Service.ApplySettingsPolicy({1, true, true, {}, {}});
        fixture.WatcherAccess->LastWatcher->Add(L"power-incoming", L"Power incoming");
        fixture.ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
        Check(StateFor(fixture.Service, L"power-incoming") == DeviceLifecycleState::Idle &&
                  !HasConnectedSession(fixture.Service),
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
                  !HasConnectedSession(fixture.Service),
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
    if (!resumedWatcher) return;

    staleWatcher->Add(L"stale-unmatched-resume", L"Stale unmatched resume");
    resumedWatcher->Add(L"fresh-unmatched-resume", L"Fresh unmatched resume");
    Check(fixture.Service.Snapshot().Inventory.Devices.size() == 1 &&
              fixture.Service.Snapshot().Inventory.Devices.front().Id == L"fresh-unmatched-resume" &&
              fixture.ConnectionAccess->Connections.empty(),
          "unmatched resume recovery must reject stale watcher callbacks without starting canceled sessions");
}

} // namespace

int RunDevicePowerTests() {
    TestResumeWatcherFailureClearsRunningStateAndAllowsRetry();
    TestBulkSuspendResumeAndShutdownCannotResurrectSessions();
    TestStartupPolicyAndDelayedPowerResume();
    TestIdleDiscoveryResumesForManualConnectAfterPowerTransition();
    TestPowerTransitionRecoveryTargetsIncludeIncomingAndPendingReconnectWithoutConnectedSessions();
    TestPowerTransitionDoesNotReleaseCloseInFlightBeforeDelayedResume();
    TestManualCommandsDuringDelayedPowerRecoverySupersedeRecovery();
    TestManualCommandsSupersedeDelayedPowerRecoveryDuringCloseBarrier();
    TestPowerTransitionRecoveryCaptureRejectsStaleDelayedIntent();
    TestUnmatchedPowerResumeRestartsWatcherWithoutResurrectingSessions();
    return g_failures;
}
