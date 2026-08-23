#include <pch.h>

#include <core/DeviceWatcher.hpp>

#include <stdexcept>
#include <utility>

namespace apc::device {
namespace {

device_picker::DeviceInventorySnapshot BuildSnapshot(std::uint64_t inventoryGeneration,
                                                     bool enumerationComplete,
                                                     std::unordered_map<std::wstring, std::wstring> const& devices) {
    device_picker::DeviceInventorySnapshot snapshot;
    snapshot.Generation = inventoryGeneration;
    snapshot.EnumerationComplete = enumerationComplete;
    snapshot.Devices.reserve(devices.size());
    for (auto const& [id, name] : devices) {
        snapshot.Devices.push_back({id, name});
    }
    std::ranges::sort(snapshot.Devices, [](auto const& left, auto const& right) {
        auto const leftName = left.Name.empty() ? std::wstring_view(left.Id) : std::wstring_view(left.Name);
        auto const rightName = right.Name.empty() ? std::wstring_view(right.Id) : std::wstring_view(right.Name);
        if (leftName != rightName) return leftName < rightName;
        return left.Id < right.Id;
    });
    return snapshot;
}

class WindowsDeviceInformationWatcher final : public DeviceWatcherRegistration {
public:
    explicit WindowsDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) {
        auto const selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
        m_watcher = winrt::Windows::Devices::Enumeration::DeviceInformation::CreateWatcher(selector);

        m_addedToken = m_watcher.Added([deviceAdded = std::move(callbacks.DeviceAdded)](auto const&, auto const& args) {
            if (deviceAdded) deviceAdded({std::wstring(args.Id()), std::wstring(args.Name())});
        });
        m_removedToken =
            m_watcher.Removed([deviceRemoved = std::move(callbacks.DeviceRemoved)](auto const&, auto const& args) {
                if (deviceRemoved) deviceRemoved(std::wstring(args.Id()));
            });
        m_enumerationCompletedToken = m_watcher.EnumerationCompleted(
            [enumerationCompleted = std::move(callbacks.EnumerationCompleted)](auto const&, auto const&) {
                if (enumerationCompleted) enumerationCompleted();
            });
    }

    ~WindowsDeviceInformationWatcher() override {
        Stop();
        RevokeCallbacks();
    }

    void Start() override { m_watcher.Start(); }

    void Stop() noexcept override {
        if (!m_watcher) return;
        try {
            m_watcher.Stop();
        } catch (winrt::hresult_error const& error) {
            util::DebugTraceException(L"[DeviceWatcher] failed to stop DeviceInformation watcher", error);
        } catch (std::exception const& error) {
            util::DebugTraceException(L"[DeviceWatcher] failed to stop DeviceInformation watcher", error);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceWatcher] failed to stop DeviceInformation watcher");
        }
    }

    void RevokeCallbacks() noexcept override {
        if (!m_watcher) return;
        Revoke(m_addedToken, [this](winrt::event_token token) { m_watcher.Added(token); });
        Revoke(m_removedToken, [this](winrt::event_token token) { m_watcher.Removed(token); });
        Revoke(m_enumerationCompletedToken,
               [this](winrt::event_token token) { m_watcher.EnumerationCompleted(token); });
    }

private:
    template <typename Revoker> void Revoke(winrt::event_token& token, Revoker&& revoke) noexcept {
        if (token.value == 0) return;
        auto const tokenToRevoke = std::exchange(token, {});
        try {
            std::forward<Revoker>(revoke)(tokenToRevoke);
        } catch (winrt::hresult_error const& error) {
            util::DebugTraceException(L"[DeviceWatcher] failed to revoke DeviceInformation watcher callback", error);
        } catch (std::exception const& error) {
            util::DebugTraceException(L"[DeviceWatcher] failed to revoke DeviceInformation watcher callback", error);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceWatcher] failed to revoke DeviceInformation watcher callback");
        }
    }

    winrt::Windows::Devices::Enumeration::DeviceWatcher m_watcher{nullptr};
    winrt::event_token m_addedToken{};
    winrt::event_token m_removedToken{};
    winrt::event_token m_enumerationCompletedToken{};
};

class WindowsDeviceWatcherPlatform final : public DeviceWatcherPlatform {
public:
    [[nodiscard]] std::unique_ptr<DeviceWatcherRegistration>
    CreateDeviceInformationWatcher(DeviceWatcherCallbacks callbacks) override {
        return std::make_unique<WindowsDeviceInformationWatcher>(std::move(callbacks));
    }
};

} // namespace

struct DeviceWatcher::State : std::enable_shared_from_this<DeviceWatcher::State> {
    SerializedExecutor Executor;
    FactSink PublishFact;
    std::unique_ptr<DeviceWatcherPlatform> Platform;
    std::unique_ptr<DeviceWatcherRegistration> Registration;
    std::unordered_map<std::wstring, std::wstring> Devices;
    std::uint64_t WatcherGeneration = 0;
    std::uint64_t InventoryGeneration = 0;
    bool IsWatcherRunning = false;
    bool IsEnumerationComplete = false;
    bool IsShutdown = false;

    [[nodiscard]] device_picker::DeviceInventorySnapshot Snapshot() const {
        return BuildSnapshot(InventoryGeneration, IsEnumerationComplete, Devices);
    }

    void Publish(DeviceWatcherFactKind kind, std::wstring deviceId = {}, std::wstring deviceName = {}) {
        if (!PublishFact) return;
        DeviceWatcherFact fact;
        fact.Kind = kind;
        fact.WatcherGeneration = WatcherGeneration;
        fact.Inventory = Snapshot();
        fact.DeviceId = std::move(deviceId);
        fact.DeviceName = std::move(deviceName);
        PublishFact(fact);
    }

    void PostDeviceAdded(std::uint64_t callbackGeneration, device_picker::DeviceIdentity device) {
        auto weak = weak_from_this();
        Executor([weak, callbackGeneration, device = std::move(device)]() mutable {
            if (auto state = weak.lock()) state->OnDeviceAdded(callbackGeneration, std::move(device));
        });
    }

    void PostDeviceRemoved(std::uint64_t callbackGeneration, std::wstring deviceId) {
        auto weak = weak_from_this();
        Executor([weak, callbackGeneration, deviceId = std::move(deviceId)]() mutable {
            if (auto state = weak.lock()) state->OnDeviceRemoved(callbackGeneration, std::move(deviceId));
        });
    }

    void PostEnumerationCompleted(std::uint64_t callbackGeneration) {
        auto weak = weak_from_this();
        Executor([weak, callbackGeneration] {
            if (auto state = weak.lock()) state->OnEnumerationCompleted(callbackGeneration);
        });
    }

    void OnDeviceAdded(std::uint64_t callbackGeneration, device_picker::DeviceIdentity device) {
        if (!Accepts(callbackGeneration)) return;
        auto [entry, inserted] = Devices.try_emplace(device.Id, device.Name);
        bool const inventoryChanged = inserted || entry->second != device.Name;
        if (inventoryChanged) {
            entry->second = device.Name;
            ++InventoryGeneration;
        }
        Publish(DeviceWatcherFactKind::DeviceAdded, std::move(device.Id), std::move(device.Name));
        if (inventoryChanged) Publish(DeviceWatcherFactKind::InventoryChanged);
    }

    void OnDeviceRemoved(std::uint64_t callbackGeneration, std::wstring deviceId) {
        if (!Accepts(callbackGeneration)) return;
        bool const inventoryChanged = Devices.erase(deviceId) != 0;
        if (inventoryChanged) ++InventoryGeneration;
        Publish(DeviceWatcherFactKind::DeviceRemoved, std::move(deviceId));
        if (inventoryChanged) Publish(DeviceWatcherFactKind::InventoryChanged);
    }

    void OnEnumerationCompleted(std::uint64_t callbackGeneration) {
        if (!Accepts(callbackGeneration) || IsEnumerationComplete) return;
        IsEnumerationComplete = true;
        ++InventoryGeneration;
        Publish(DeviceWatcherFactKind::EnumerationCompleted);
        Publish(DeviceWatcherFactKind::InventoryChanged);
    }

    [[nodiscard]] bool Accepts(std::uint64_t callbackGeneration) const noexcept {
        return !IsShutdown && IsWatcherRunning && callbackGeneration == WatcherGeneration;
    }

    void StopRegistration() noexcept {
        auto registration = std::move(Registration);
        if (!registration) return;
        registration->Stop();
        registration->RevokeCallbacks();
    }
};

DeviceWatcher::DeviceWatcher(SerializedExecutor serializedExecutor,
                             FactSink factSink,
                             std::unique_ptr<DeviceWatcherPlatform> platform)
    : m_state(std::make_shared<State>()) {
    if (!serializedExecutor) throw std::invalid_argument("DeviceWatcher requires a serialized executor");
    m_state->Executor = std::move(serializedExecutor);
    m_state->PublishFact = std::move(factSink);
    m_state->Platform = platform ? std::move(platform) : std::make_unique<WindowsDeviceWatcherPlatform>();
}

DeviceWatcher::~DeviceWatcher() {
    Shutdown();
}

bool DeviceWatcher::Start() {
    auto const state = m_state;
    if (!state || state->IsShutdown || state->IsWatcherRunning) return false;

    auto const watcherGeneration = ++state->WatcherGeneration;
    state->IsWatcherRunning = true;
    state->IsEnumerationComplete = false;
    state->Devices.clear();
    ++state->InventoryGeneration;

    DeviceWatcherCallbacks callbacks;
    auto weak = std::weak_ptr<State>(state);
    callbacks.DeviceAdded = [weak, watcherGeneration](device_picker::DeviceIdentity device) mutable {
        if (auto callbackState = weak.lock()) callbackState->PostDeviceAdded(watcherGeneration, std::move(device));
    };
    callbacks.DeviceRemoved = [weak, watcherGeneration](std::wstring deviceId) mutable {
        if (auto callbackState = weak.lock()) callbackState->PostDeviceRemoved(watcherGeneration, std::move(deviceId));
    };
    callbacks.EnumerationCompleted = [weak, watcherGeneration] {
        if (auto callbackState = weak.lock()) callbackState->PostEnumerationCompleted(watcherGeneration);
    };

    std::unique_ptr<DeviceWatcherRegistration> registration;
    try {
        registration = state->Platform->CreateDeviceInformationWatcher(std::move(callbacks));
        if (!registration) throw std::runtime_error("DeviceWatcher platform returned no registration");
        registration->Start();
        if (!state->Accepts(watcherGeneration)) {
            registration->Stop();
            registration->RevokeCallbacks();
            return false;
        }
        state->Registration = std::move(registration);
        state->Publish(DeviceWatcherFactKind::InventoryChanged);
        return true;
    } catch (winrt::hresult_error const& error) {
        util::DebugTraceException(L"[DeviceWatcher] failed to create or start DeviceInformation watcher", error);
    } catch (std::exception const& error) {
        util::DebugTraceException(L"[DeviceWatcher] failed to create or start DeviceInformation watcher", error);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DeviceWatcher] failed to create or start DeviceInformation watcher");
    }

    if (state->WatcherGeneration == watcherGeneration) {
        state->IsWatcherRunning = false;
        state->IsEnumerationComplete = false;
        ++state->WatcherGeneration;
    }
    if (registration) {
        registration->Stop();
        registration->RevokeCallbacks();
    }
    state->Publish(DeviceWatcherFactKind::InventoryChanged);
    return false;
}

void DeviceWatcher::Stop() noexcept {
    auto const state = m_state;
    if (!state || !state->IsWatcherRunning) return;

    state->IsWatcherRunning = false;
    state->IsEnumerationComplete = false;
    ++state->WatcherGeneration;
    ++state->InventoryGeneration;
    state->StopRegistration();
}

void DeviceWatcher::Shutdown() noexcept {
    auto const state = std::exchange(m_state, {});
    if (!state || state->IsShutdown) return;

    state->IsShutdown = true;
    state->IsWatcherRunning = false;
    state->IsEnumerationComplete = false;
    ++state->WatcherGeneration;
    state->StopRegistration();
}

device_picker::DeviceInventorySnapshot DeviceWatcher::Snapshot() const {
    if (!m_state) return {};
    return m_state->Snapshot();
}

bool DeviceWatcher::IsRunning() const noexcept {
    return m_state && m_state->IsWatcherRunning;
}

std::uint64_t DeviceWatcher::Generation() const noexcept {
    return m_state ? m_state->WatcherGeneration : 0;
}

} // namespace apc::device
