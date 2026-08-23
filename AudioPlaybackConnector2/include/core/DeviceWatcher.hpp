#pragma once

#include <core/DevicePickerTypes.hpp>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace apc::device {

struct DeviceWatcherCallbacks {
    std::function<void(device_picker::DeviceIdentity)> DeviceAdded;
    std::function<void(std::wstring)> DeviceRemoved;
    std::function<void()> EnumerationCompleted;
};

class DeviceWatcherRegistration {
public:
    virtual ~DeviceWatcherRegistration() = default;

    virtual void Start() = 0;
    virtual void Stop() noexcept = 0;
    virtual void RevokeCallbacks() noexcept = 0;
};

class DeviceWatcherPlatform {
public:
    virtual ~DeviceWatcherPlatform() = default;

    [[nodiscard]] virtual std::unique_ptr<DeviceWatcherRegistration>
    CreateDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) = 0;
};

enum class DeviceWatcherFactKind { DeviceAdded, DeviceRemoved, EnumerationCompleted, InventoryChanged };

struct DeviceWatcherFact {
    DeviceWatcherFactKind Kind = DeviceWatcherFactKind::InventoryChanged;
    std::uint64_t WatcherGeneration = 0;
    device_picker::DeviceInventorySnapshot Inventory;
    std::wstring DeviceId;
    std::wstring DeviceName;
};

// DeviceService owns the serialized device context. Every public operation and fact sink invocation runs there;
// platform callbacks only enqueue work onto that context.
class DeviceWatcher {
public:
    using Task = std::function<void()>;
    using SerializedExecutor = std::function<void(Task)>;
    using FactSink = std::function<void(DeviceWatcherFact const&)>;

    explicit DeviceWatcher(SerializedExecutor serializedExecutor,
                           FactSink factSink = {},
                           std::unique_ptr<DeviceWatcherPlatform> platform = {});
    ~DeviceWatcher();

    DeviceWatcher(DeviceWatcher const&) = delete;
    DeviceWatcher& operator=(DeviceWatcher const&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    void Shutdown() noexcept;
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshAsync();

    [[nodiscard]] device_picker::DeviceInventorySnapshot Snapshot() const;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;

private:
    struct State;

    std::shared_ptr<State> m_state;
};

} // namespace apc::device
