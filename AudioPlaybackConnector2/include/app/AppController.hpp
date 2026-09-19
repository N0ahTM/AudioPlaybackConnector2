#pragma once

#include <app/AppModels.hpp>
#include <app/DeviceFactPublicationFence.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace apc::device {
class DeviceService;
}

namespace apc::app {

// The controller is a transport- and UI-free application boundary.
class AppController final {
    struct EventState;

public:
    using Executor = std::function<AppResult(AppCommand const&, AppCommandContext const&)>;
    using SnapshotProvider = std::function<AppSnapshot()>;
    struct EventNotification {
        std::uint64_t Revision = 0;
        AppEvent Event;
        std::optional<DeviceFactPublicationFence::Token> DeviceToken;
    };
    using EventHandler = std::function<void(EventNotification const&)>;
    using SubscriptionId = std::uint64_t;

    class Subscription final {
    public:
        Subscription() noexcept = default;
        ~Subscription();

        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        void Reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

    private:
        friend class AppController;

        Subscription(std::weak_ptr<EventState> state, SubscriptionId id) noexcept
            : m_state(std::move(state)), m_id(id) {}

        std::weak_ptr<EventState> m_state;
        SubscriptionId m_id = 0;
    };

    AppController(Executor executor,
                  SnapshotProvider snapshotProvider,
                  std::shared_ptr<apc::device::DeviceService> devices = {});
    ~AppController();

    AppController(AppController const&) = delete;
    AppController& operator=(AppController const&) = delete;
    AppController(AppController&&) = delete;
    AppController& operator=(AppController&&) = delete;

    [[nodiscard]] AppSnapshot Snapshot() const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// UI Actions ////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult ShowDevicePicker(DevicePickerOpenMode mode, AppCommandContext context = {}) const noexcept;
    [[nodiscard]] AppResult ShowSettings(AppCommandContext context = {}) const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Actions ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult Connect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult Disconnect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult Reconnect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult ToggleDefault(AppCommandContext context) const;
    [[nodiscard]] AppResult Toggle(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult DisconnectAll(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ReconnectAll(AppCommandContext context) const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Settings ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult SetDefault(std::wstring_view deviceId) const;
    [[nodiscard]] AppResult ClearDefault(AppCommandContext context = {}) const;
    [[nodiscard]] AppResult SetAlias(std::wstring_view deviceId, std::wstring_view alias) const;
    [[nodiscard]] AppResult ClearAlias(std::wstring_view deviceId) const;
    [[nodiscard]] AppResult SetDefault(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult SetAlias(DeviceSelector target, std::wstring_view alias, AppCommandContext context) const;
    [[nodiscard]] AppResult ClearAlias(DeviceSelector target, AppCommandContext context) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Queries ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult ListDevices(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult Status(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ShowDefault(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ListAliases(AppCommandContext context) const noexcept;

    // Publications have one total revision order. Reentrant and concurrent publications queue behind the current
    // delivery; callbacks never overlap. Reset drains an admitted callback, except when called by that callback.
    [[nodiscard]] Subscription Subscribe(EventHandler handler);
    void Publish(AppEvent const& event) const noexcept;
    [[nodiscard]] bool IsCurrent(EventNotification const& notification) const;

private:
    [[nodiscard]] AppResult Execute(AppCommand command, AppCommandContext context = {}) const noexcept;

    Executor m_executor;
    SnapshotProvider m_snapshotProvider;
    std::shared_ptr<EventState> m_eventState;
    std::shared_ptr<apc::device::DeviceService> m_devices;
    std::uint64_t m_deviceSubscription = 0;
};

} // namespace apc::app
