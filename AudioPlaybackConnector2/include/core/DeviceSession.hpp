#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace apc::device {

enum class DeviceLifecycleState { Idle, Connecting, Connected, Disconnecting, WaitingForReconnect, Failed };
enum class DeviceOperationKind { ManualConnect, ManualReconnect, AutomaticReconnect, Startup, Resume, IncomingEnable };
enum class DeviceConnectionState { Opened, Closed };
enum class DeviceConnectionResult { Success, TimedOut, Denied, Failed, Cancelled };

struct DeviceSessionSnapshot {
    std::wstring DeviceId;
    std::wstring DeviceName;
    DeviceLifecycleState State = DeviceLifecycleState::Idle;
    std::uint64_t OperationEpoch = 0;
    std::size_t CompletedRetryAttempts = 0;
    bool HasConnection = false;
    bool IsIncomingEnabled = false;
    bool IsReconnectEnabled = true;
    bool IsReconnectCancelled = false;
};

struct DeviceSessionFact {
    DeviceSessionSnapshot Snapshot;
    DeviceOperationKind Operation = DeviceOperationKind::ManualConnect;
    DeviceConnectionResult Result = DeviceConnectionResult::Success;
    bool IsTerminalFailure = false;
};

class DeviceConnection {
public:
    using StateChangedHandler = std::function<void(DeviceConnectionState)>;
    using Completion = std::function<void(DeviceConnectionResult)>;
    using CloseCompletion = std::function<void()>;

    virtual ~DeviceConnection() = default;
    [[nodiscard]] virtual std::uint64_t RegisterStateChanged(StateChangedHandler handler) = 0;
    virtual void RevokeStateChanged(std::uint64_t token) noexcept = 0;
    virtual void Start(Completion completion) = 0;
    virtual void Open(Completion completion) = 0;
    virtual void Close(CloseCompletion completion) noexcept = 0;
};

class DeviceConnectionPlatform {
public:
    virtual ~DeviceConnectionPlatform() = default;
    [[nodiscard]] virtual std::unique_ptr<DeviceConnection> Create(std::wstring const& deviceId) = 0;
};

class DeviceTimer {
public:
    virtual ~DeviceTimer() = default;
    virtual void Cancel() noexcept = 0;
};

class DeviceTimerPlatform {
public:
    using Callback = std::function<void()>;

    virtual ~DeviceTimerPlatform() = default;
    [[nodiscard]] virtual std::unique_ptr<DeviceTimer> Schedule(std::chrono::milliseconds delay, Callback callback) = 0;
};

[[nodiscard]] std::unique_ptr<DeviceConnectionPlatform> CreateWindowsDeviceConnectionPlatform();
[[nodiscard]] std::unique_ptr<DeviceTimerPlatform> CreateWindowsDeviceTimerPlatform();

// DeviceService exclusively calls these operations on its serialized device context. Platform callbacks only
// enqueue a guarded continuation through the executor supplied at construction.
class DeviceSession : public std::enable_shared_from_this<DeviceSession> {
public:
    using Task = std::function<void()>;
    using SerializedExecutor = std::function<void(Task)>;
    using FactSink = std::function<void(DeviceSessionFact const&)>;

    DeviceSession(std::wstring deviceId,
                  std::wstring deviceName,
                  SerializedExecutor serializedExecutor,
                  DeviceConnectionPlatform& connectionPlatform,
                  DeviceTimerPlatform& timerPlatform,
                  FactSink factSink);
    ~DeviceSession();

    DeviceSession(DeviceSession const&) = delete;
    DeviceSession& operator=(DeviceSession const&) = delete;

    [[nodiscard]] DeviceSessionSnapshot Snapshot() const;
    [[nodiscard]] bool IsBusy() const noexcept;
    [[nodiscard]] bool IsSuspended() const noexcept;
    void Rename(std::wstring deviceName);
    void HandleDeviceRemoved();
    void Connect(DeviceOperationKind operation, bool openImmediately);
    void Disconnect(bool restoreIncoming);
    void CancelReconnect();
    void SetReconnectEnabled(bool enabled);
    void SetIncomingEnabled(bool enabled);
    void Suspend();
    void Resume();
    void ResumeIdleAfterPowerTransition();
    void Shutdown() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace apc::device
