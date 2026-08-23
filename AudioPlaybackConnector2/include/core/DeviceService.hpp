#pragma once

#include <core/DevicePickerTypes.hpp>
#include <core/DeviceSession.hpp>
#include <core/DeviceWatcher.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace apc::device
