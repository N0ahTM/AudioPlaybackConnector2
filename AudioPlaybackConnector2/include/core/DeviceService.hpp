#pragma once

#include <core/DevicePickerTypes.hpp>
#include <core/DeviceTrayPresentation.hpp>
#include <core/DeviceSession.hpp>
#include <core/DeviceWatcher.hpp>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
enum class DeviceFactKind { InventoryChanged, SessionChanged, OperationFailed, Shutdown };

struct DeviceCommandResult {
    DeviceCommandKind Command = DeviceCommandKind::Connect;
    DeviceCommandResultKind Kind = DeviceCommandResultKind::Rejected;
    std::wstring DeviceId;
    std::uint64_t OperationEpoch = 0;
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
    bool IsTerminalFailure = false;
};

struct DeviceServiceDependencies {
    std::unique_ptr<DeviceWatcherPlatform> WatcherPlatform;
    std::unique_ptr<DeviceConnectionPlatform> ConnectionPlatform;
    std::unique_ptr<DeviceTimerPlatform> TimerPlatform;
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
    void ConfigureIncomingConnections(bool enabled);
    void ConfigureReconnectPolicy(bool globallyEnabled, std::vector<std::wstring> enabledDeviceIds);
    void ConnectStartupTargets(std::vector<std::wstring> deviceIds);
    void Suspend();
    void Resume();
    void Shutdown() noexcept;
    [[nodiscard]] DeviceServiceSnapshot Snapshot() const;

    // Transitional composition entry points. They forward into this owner;
    // they do not create a second device-state pipeline and are removed when
    // AppController takes over the application command surface.
    void StartDeviceWatcher();
    void StopDeviceWatcher();
    void ShutdownForProcessExit() noexcept;
    void SuspendForPowerTransition() noexcept;
    void ResumeAfterPowerTransition();
    void ResumeSuspendedSessions(std::vector<std::wstring> deviceIds);
    void SetIncomingConnectionsEnabled(bool enabled);
    void ApplyReconnectOnConnectionLossPolicy(bool globallyEnabled,
                                              std::span<const std::wstring> individuallyEnabledDeviceIds);
    void SetReconnectOnConnectionLoss(std::wstring deviceId, bool enabled);
    void SetReconnectOnConnectionLoss(winrt::hstring deviceId, bool enabled) {
        SetReconnectOnConnectionLoss(std::wstring(deviceId), enabled);
    }
    winrt::Windows::Foundation::IAsyncAction ConnectAsync(winrt::hstring deviceId);
    void ConnectDetached(winrt::hstring deviceId);
    winrt::Windows::Foundation::IAsyncAction ReconnectAsync(winrt::hstring deviceId);
    void ReconnectDetached(winrt::hstring deviceId);
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshDevicesAsync();
    [[nodiscard]] std::vector<DeviceSessionSnapshot> GetConnectedDevices() const;
    [[nodiscard]] std::vector<DeviceSessionSnapshot> GetConnectionSessions() const;
    [[nodiscard]] std::vector<std::wstring> GetPowerTransitionRecoveryDeviceIds() const;
    [[nodiscard]] bool IsDeviceConnected(std::wstring_view deviceId) const;
    [[nodiscard]] bool IsDeviceConnected(winrt::hstring const& deviceId) const {
        return IsDeviceConnected(std::wstring(deviceId));
    }
    [[nodiscard]] std::optional<std::wstring> GetConnectionDisplayName(std::wstring_view deviceId) const;
    [[nodiscard]] std::optional<std::wstring> GetConnectionDisplayName(winrt::hstring const& deviceId) const {
        return GetConnectionDisplayName(std::wstring(deviceId));
    }
    [[nodiscard]] bool HasConnections() const;
    [[nodiscard]] bool HasBusyOperations() const;
    [[nodiscard]] bool IsDeviceBusy(std::wstring_view deviceId) const;
    [[nodiscard]] bool IsDeviceBusy(winrt::hstring const& deviceId) const {
        return IsDeviceBusy(std::wstring(deviceId));
    }
    [[nodiscard]] device_picker::DeviceActivitySnapshot GetDevicePickerActivitySnapshot() const;
    [[nodiscard]] device_picker::DeviceInventorySnapshot GetDevicePickerInventorySnapshot() const;
    [[nodiscard]] std::optional<device_picker::DeviceInventorySnapshot>
    GetDevicePickerInventorySnapshotIfChanged(std::uint64_t knownGeneration) const;
    [[nodiscard]] DeviceTrayPresentationSnapshot GetTrayPresentationSnapshot() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace apc::device
