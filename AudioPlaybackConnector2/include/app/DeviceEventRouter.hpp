#pragma once

#include <functional>
#include <memory>

class DeviceManager;
enum class DeviceStatusKind;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Event Router ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceEventRouter {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using UiDispatcher = std::function<bool(std::function<void()> work)>;

    struct Callbacks {
        std::function<void(winrt::hstring const& id)> DeviceConnected;
        std::function<void(winrt::hstring const& id)> DeviceDisconnected;
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

    void Attach(std::shared_ptr<DeviceManager> deviceManager, UiDispatcher dispatcher, Callbacks callbacks);
    void Detach() noexcept;

private:
    struct State;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] static bool Dispatch(std::shared_ptr<State> const& state, std::function<void()> work) noexcept;
    void ResetTokens() noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::shared_ptr<DeviceManager> m_deviceManager;
    std::shared_ptr<State> m_state;

    std::size_t m_deviceConnectedToken = 0;
    std::size_t m_deviceDisconnectedToken = 0;
    std::size_t m_connectionErrorToken = 0;
    std::size_t m_autoReconnectTriggeredToken = 0;
    std::size_t m_autoReconnectFailedToken = 0;
    std::size_t m_deviceStatusChangedToken = 0;
    std::size_t m_deviceActivityChangedToken = 0;
    std::size_t m_deviceInventoryChangedToken = 0;
};
