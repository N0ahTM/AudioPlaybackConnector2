#pragma once

#include <core/DeviceService.hpp>

#include <functional>
#include <memory>

enum class DeviceStatusKind { None, Ready, Connecting, Reconnecting, Connected, Error };

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Router ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceEventRouter {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using UiDispatcher = std::function<bool(std::function<void()> work)>;

    [[nodiscard]] static constexpr bool ShouldNotifyDisconnect(apc::device::DeviceDisconnectReason reason) noexcept {
        return reason == apc::device::DeviceDisconnectReason::UnexpectedLoss ||
               reason == apc::device::DeviceDisconnectReason::DeviceRemoved;
    }

    struct Callbacks {
        std::function<void(winrt::hstring const& id)> DeviceConnected;
        std::function<void(winrt::hstring const& id, apc::device::DeviceDisconnectReason reason)> DeviceDisconnected;
        std::function<void(winrt::hstring const& id, winrt::hstring const& message)> ConnectionError;
        std::function<void(winrt::hstring const& id)> AutoReconnectTriggered;
        std::function<void(winrt::hstring const& id)> AutoReconnectFailed;
        std::function<void(winrt::hstring const& id, winrt::hstring const& status, DeviceStatusKind statusKind)>
            DeviceStatusChanged;
        std::function<void()> DeviceActivityChanged;
        std::function<void()> DeviceInventoryChanged;
    };

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceEventRouter() = default;
    ~DeviceEventRouter();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void
    Attach(std::shared_ptr<apc::device::DeviceService> deviceService, UiDispatcher dispatcher, Callbacks callbacks);
    void Detach() noexcept;

private:
    struct State;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] static bool Dispatch(std::shared_ptr<State> const& state, std::function<void()> work) noexcept;
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::shared_ptr<apc::device::DeviceService> m_deviceService;
    std::shared_ptr<State> m_state;
    apc::device::DeviceService::Subscription m_deviceFactSubscription = 0;
};
