#include <core/DeviceOperationCoordinator.hpp>

#include <iostream>
#include <limits>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool condition, char const* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

void TestDuplicateBeginDoesNotInvalidateOwner() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;

    auto owner = coordinator.TryBegin(L"device-a", Intent::ManualConnect, Phase::Connecting);
    Check(owner.has_value(), "the first operation must acquire the device");
    Check(!coordinator.TryBegin(L"device-a", Intent::ManualConnect, Phase::Connecting),
          "a duplicate operation must be coalesced");
    Check(!coordinator.TryBegin(L"device-a", Intent::ManualReconnect, Phase::Reconnecting),
          "a conflicting operation must be coalesced");
    Check(owner && coordinator.IsCurrent(*owner), "a rejected duplicate must not invalidate the owner");
    Check(owner && coordinator.Complete(*owner), "the owner must complete its reservation");
    Check(!coordinator.HasActiveOperations(), "completion must remove active state rather than retain history");
}

void TestTransitionAndStaleCompletion() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;

    auto reconnect = coordinator.TryBegin(L"device-b", Intent::ManualConnect, Phase::Connecting);
    Check(reconnect && coordinator.Transition(*reconnect, Phase::Connecting, Phase::Reconnecting),
          "connect must transition atomically along the production phase edge");
    Check(coordinator.IsInPhase(L"device-b", Phase::Reconnecting), "the transitioned phase must be observable");
    Check(reconnect && !coordinator.Transition(*reconnect, Phase::Connecting, Phase::Reconnecting),
          "a wrong-phase transition must be rejected");

    Check(coordinator.Invalidate(L"device-b"), "invalidation must remove the active operation");
    auto replacement = coordinator.TryBegin(L"device-b", Intent::ManualConnect, Phase::Connecting);
    Check(replacement.has_value(), "a replacement must start after invalidation");
    Check(reconnect && !coordinator.Complete(*reconnect), "stale completion must not erase a replacement");
    Check(replacement && coordinator.IsCurrent(*replacement), "replacement must survive stale completion");
    Check(replacement && coordinator.Complete(*replacement), "replacement must complete normally");
    Check(replacement && !coordinator.Complete(*replacement), "double completion must be a no-op");
}

void TestIndependentDevicesAndCancelAll() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;

    auto first = coordinator.TryBegin(L"device-c", Intent::AutoReconnect, Phase::Connecting);
    auto second = coordinator.TryBegin(L"device-d", Intent::IncomingEnable, Phase::Connecting);
    Check(first && second, "different devices must operate independently");
    Check(coordinator.Snapshot().size() == 2, "snapshot must contain active operations only");

    coordinator.CancelAll();
    Check(!coordinator.HasActiveOperations(), "cancel-all must remove all active operations");
    Check(first && !coordinator.IsCurrent(*first), "cancel-all must make every prior token stale");
    Check(second && !coordinator.IsCurrent(*second), "cancel-all must invalidate every device token");
    auto replacement = coordinator.TryBegin(L"device-c", Intent::ManualConnect, Phase::Connecting);
    Check(first && replacement && first->Id != replacement->Id,
          "a device restarted after cancel-all must receive a new token identity");
}

void TestFailureReportCanBeClaimedExactlyOnce() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;

    auto operation = coordinator.TryBegin(L"device-f", Intent::ManualConnect, Phase::Connecting);
    Check(operation && coordinator.TryClaimFailureReport(*operation),
          "the current operation must claim failure publication");
    Check(operation && !coordinator.TryClaimFailureReport(*operation),
          "the same operation must not claim failure publication twice");
    Check(operation && coordinator.IsCurrent(*operation),
          "failure publication must keep the operation active as an ordering fence");
    Check(operation && !coordinator.Transition(*operation, Phase::Connecting, Phase::Reconnecting),
          "a claimed failure report must prevent further operation transitions");
    Check(operation && coordinator.Complete(*operation), "the failure reporter must complete its operation");
    Check(operation && !coordinator.TryClaimFailureReport(*operation),
          "a completed operation must not claim another failure publication");

    auto replacement = coordinator.TryBegin(L"device-f", Intent::ManualConnect, Phase::Connecting);
    Check(operation && replacement && *operation != *replacement, "a replacement must have a distinct token identity");
    Check(operation && !coordinator.TryClaimFailureReport(*operation),
          "a stale token must not claim failure publication from its replacement");
    Check(replacement && coordinator.TryClaimFailureReport(*replacement),
          "the replacement must own its independent failure publication");
}

void TestInvalidInputsAreNoOps() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;
    Check(!coordinator.TryBegin(L"", Intent::ManualConnect, Phase::Connecting), "an empty device id must be rejected");
    Check(!coordinator.Invalidate(L"missing"), "invalidating an idle device must be a no-op");
    Check(!coordinator.HasActiveOperations(), "invalid inputs must not create state");
}

void TestTokenExhaustionFailsClosed() {
    using namespace apc::device_operation;
    DeviceOperationCoordinator coordinator;
    coordinator.SetLastTokenIdForTesting(std::numeric_limits<std::uint64_t>::max());
    Check(!coordinator.TryBegin(L"device-e", Intent::ManualConnect, Phase::Connecting),
          "token exhaustion must reject new operations rather than wrap");
    Check(!coordinator.HasActiveOperations(), "token exhaustion must not create partial active state");
}

} // namespace

int RunDeviceOperationCoordinatorTests() {
    TestDuplicateBeginDoesNotInvalidateOwner();
    TestTransitionAndStaleCompletion();
    TestIndependentDevicesAndCancelAll();
    TestFailureReportCanBeClaimedExactlyOnce();
    TestInvalidInputsAreNoOps();
    TestTokenExhaustionFailsClosed();
    return g_failures;
}
