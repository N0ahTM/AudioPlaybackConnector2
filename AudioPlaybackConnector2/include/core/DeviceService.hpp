#pragma once

#include <core/DevicePickerTypes.hpp>
#include <core/DeviceSession.hpp>
#include <core/DeviceWatcher.hpp>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace apc::device {

enum class DeviceCommandKind {
    Connect,
    Disconnect,
    Reconnect,
    DisconnectAll,
    ReconnectAll,
    Start,
    Stop,
    Suspend,
    Resume,
    Shutdown
};
enum class DeviceCommandResultKind { Accepted, Coalesced, Rejected, Cancelled };
enum class DeviceOperationStatus { Succeeded, Failed, Cancelled, TimedOut, Rejected };
enum class DeviceFactKind { InventoryChanged, SessionChanged, OperationFailed, Shutdown };

struct DeviceOperationCompletion;

struct DeviceCommandResult {
    DeviceCommandKind Command = DeviceCommandKind::Connect;
    DeviceCommandResultKind Kind = DeviceCommandResultKind::Rejected;
    std::wstring DeviceId;
    std::uint64_t OperationEpoch = 0;
    std::shared_ptr<DeviceOperationCompletion> Completion;
};

struct DeviceServiceSnapshot {
    std::uint64_t Generation = 0;
    bool IsRunning = false;
    bool IsSuspended = false;
    bool IsShutdown = false;
    device_picker::DeviceInventorySnapshot Inventory;
    std::vector<DeviceSessionSnapshot> Sessions;
};

struct DeviceFact {
    DeviceFactKind Kind = DeviceFactKind::SessionChanged;
    DeviceServiceSnapshot Snapshot;
    std::wstring DeviceId;
    DeviceConnectionResult ConnectionResult = DeviceConnectionResult::Success;
    DeviceOperationKind Operation = DeviceOperationKind::ManualConnect;
    bool IsTerminalFailure = false;
    DeviceDisconnectReason DisconnectReason = DeviceDisconnectReason::None;
};

struct DeviceServiceDependencies {
    util::LogSink Log;
    std::unique_ptr<DeviceWatcherPlatform> WatcherPlatform;
    std::unique_ptr<DeviceConnectionPlatform> ConnectionPlatform;
    std::unique_ptr<DeviceTimerPlatform> TimerPlatform;
};

struct DeviceSettingsPolicy {
    std::uint64_t Revision = 0;
    bool AllowIncomingConnections = false;
    bool GlobalReconnectOnConnectionLoss = false;
    std::vector<std::wstring> ReconnectDeviceIds;
    std::stop_token StopToken;
};

// DeviceService owns the only session map and its concrete serialized context. The fact subscriber is invoked on
// that context after each mutation, without holding the context queue lock.
class DeviceService {
public:
    using FactSink = std::function<void(DeviceFact const&)>;
    using Subscription = std::uint64_t;

    explicit DeviceService(DeviceServiceDependencies dependencies = {});
    ~DeviceService();

    DeviceService(DeviceService const&) = delete;
    DeviceService& operator=(DeviceService const&) = delete;

    [[nodiscard]] Subscription Subscribe(FactSink factSink);
    void Unsubscribe(Subscription subscription) noexcept;
    [[nodiscard]] DeviceCommandResult Start();
    [[nodiscard]] DeviceCommandResult Stop();
    [[nodiscard]] DeviceCommandResult Connect(std::wstring deviceId);
    [[nodiscard]] DeviceCommandResult Disconnect(std::wstring deviceId);
    [[nodiscard]] DeviceCommandResult Reconnect(std::wstring deviceId);
    [[nodiscard]] DeviceCommandResult CancelReconnect(std::wstring deviceId);
    [[nodiscard]] DeviceCommandResult CancelPendingReconnects();
    [[nodiscard]] DeviceCommandResult DisconnectAll();
    [[nodiscard]] DeviceCommandResult ReconnectAll();
    void ApplySettingsPolicy(DeviceSettingsPolicy policy);
    void ConnectStartupTargets(std::vector<std::wstring> deviceIds);
    void Suspend();
    void Resume();
    void Shutdown() noexcept;
    [[nodiscard]] DeviceServiceSnapshot Snapshot() const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Power Recovery ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::vector<std::wstring> SuspendForPowerTransition();
    void ResumeAfterPowerTransition();
    void ResumeSuspendedSessions(std::vector<std::wstring> deviceIds);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Operation Completion //////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] DeviceOperationStatus
    WaitForCompletion(DeviceCommandResult const& command,
                      std::stop_token stopToken = {},
                      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshDevicesAsync();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Snapshot Queries //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] std::vector<std::wstring> GetPowerTransitionRecoveryDeviceIds() const;
    [[nodiscard]] bool IsDeviceConnected(std::wstring_view deviceId) const;
    [[nodiscard]] bool HasBusyOperations() const;
    [[nodiscard]] bool IsDeviceBusy(std::wstring_view deviceId) const;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace apc::device
