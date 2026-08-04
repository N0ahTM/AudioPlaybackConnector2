#include <pch.h>

#include <app/DeviceEventRouter.hpp>

#include <core/DeviceManager.hpp>

struct DeviceEventRouter::State {
    std::atomic<bool> active = true;
    std::atomic<bool> deviceActivityDispatchPending = false;
    std::atomic<bool> deviceInventoryDispatchPending = false;
    UiDispatcher dispatcher;
    Callbacks callbacks;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DeviceEventRouter::~DeviceEventRouter() {
    Detach();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceEventRouter::Attach(std::shared_ptr<DeviceManager> deviceManager,
                               UiDispatcher dispatcher,
                               Callbacks callbacks) {
    Detach();
    if (!deviceManager) return;

    m_deviceManager = std::move(deviceManager);
    m_state = std::make_shared<State>();
    m_state->dispatcher = std::move(dispatcher);
    m_state->callbacks = std::move(callbacks);

    auto state = m_state;
    m_deviceConnectedToken = m_deviceManager->DeviceConnected += [state](winrt::hstring id) {
        static_cast<void>(Dispatch(state, [state, id = std::move(id)]() {
            if (!state->active.load() || !state->callbacks.DeviceConnected) return;
            state->callbacks.DeviceConnected(id);
        }));
    };
    m_deviceDisconnectedToken = m_deviceManager->DeviceDisconnected += [state](winrt::hstring id) {
        static_cast<void>(Dispatch(state, [state, id = std::move(id)]() {
            if (!state->active.load() || !state->callbacks.DeviceDisconnected) return;
            state->callbacks.DeviceDisconnected(id);
        }));
    };
    m_connectionErrorToken = m_deviceManager->ConnectionError += [state](winrt::hstring id, winrt::hstring msg) {
        static_cast<void>(Dispatch(state, [state, id = std::move(id), msg = std::move(msg)]() {
            if (!state->active.load() || !state->callbacks.ConnectionError) return;
            state->callbacks.ConnectionError(id, msg);
        }));
    };
    m_autoReconnectTriggeredToken = m_deviceManager->AutoReconnectTriggered += [state](winrt::hstring id) {
        static_cast<void>(Dispatch(state, [state, id = std::move(id)]() {
            if (!state->active.load() || !state->callbacks.AutoReconnectTriggered) return;
            state->callbacks.AutoReconnectTriggered(id);
        }));
    };
    m_autoReconnectFailedToken = m_deviceManager->AutoReconnectFailed += [state](winrt::hstring id) {
        static_cast<void>(Dispatch(state, [state, id = std::move(id)]() {
            if (!state->active.load() || !state->callbacks.AutoReconnectFailed) return;
            state->callbacks.AutoReconnectFailed(id);
        }));
    };
    m_deviceStatusChangedToken = m_deviceManager->DeviceStatusChanged +=
        [state](auto id, auto status, auto, DeviceStatusKind statusKind) {
            static_cast<void>(Dispatch(state, [state, id = std::move(id), status = std::move(status), statusKind]() {
                if (!state->active.load() || !state->callbacks.DeviceStatusChanged) return;
                state->callbacks.DeviceStatusChanged(id, status, statusKind);
            }));
        };
    m_deviceActivityChangedToken = m_deviceManager->DeviceActivityChanged += [state](auto id) {
        (void)id;
        if (state->deviceActivityDispatchPending.exchange(true)) return;
        if (!Dispatch(state, [state]() {
                state->deviceActivityDispatchPending.store(false);
                if (!state->active.load() || !state->callbacks.DeviceActivityChanged) return;
                state->callbacks.DeviceActivityChanged();
            })) {
            state->deviceActivityDispatchPending.store(false);
        }
    };
    m_deviceInventoryChangedToken = m_deviceManager->DeviceInventoryChanged += [state]() {
        if (state->deviceInventoryDispatchPending.exchange(true)) return;
        if (!Dispatch(state, [state]() {
                state->deviceInventoryDispatchPending.store(false);
                if (!state->active.load() || !state->callbacks.DeviceInventoryChanged) return;
                state->callbacks.DeviceInventoryChanged();
            })) {
            state->deviceInventoryDispatchPending.store(false);
        }
    };
}

void DeviceEventRouter::Detach() noexcept {
    if (m_state) {
        m_state->active.store(false);
    }

    auto deviceManager = std::exchange(m_deviceManager, nullptr);
    if (deviceManager) {
        try {
            if (m_deviceConnectedToken) {
                deviceManager->DeviceConnected -= m_deviceConnectedToken;
            }
            if (m_deviceDisconnectedToken) {
                deviceManager->DeviceDisconnected -= m_deviceDisconnectedToken;
            }
            if (m_connectionErrorToken) {
                deviceManager->ConnectionError -= m_connectionErrorToken;
            }
            if (m_autoReconnectTriggeredToken) {
                deviceManager->AutoReconnectTriggered -= m_autoReconnectTriggeredToken;
            }
            if (m_autoReconnectFailedToken) {
                deviceManager->AutoReconnectFailed -= m_autoReconnectFailedToken;
            }
            if (m_deviceStatusChangedToken) {
                deviceManager->DeviceStatusChanged -= m_deviceStatusChangedToken;
            }
            if (m_deviceActivityChangedToken) {
                deviceManager->DeviceActivityChanged -= m_deviceActivityChangedToken;
            }
            if (m_deviceInventoryChangedToken) {
                deviceManager->DeviceInventoryChanged -= m_deviceInventoryChangedToken;
            }
        } catch (...) {
            DebugTrace(L"[DeviceEventRouter] Detach ERROR: ignored exception");
        }
    }

    ResetTokens();
    m_state.reset();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool DeviceEventRouter::Dispatch(std::shared_ptr<State> const& state, std::function<void()> work) noexcept {
    if (!state || !state->active.load()) return false;
    if (state->dispatcher) {
        try {
            return state->dispatcher([state, work = std::move(work)]() mutable {
                if (!state->active.load()) return;
                try {
                    work();
                } catch (winrt::hresult_error const& ex) {
                    util::DebugTraceException(L"[DeviceEventRouter] Dispatch work ERROR", ex);
                } catch (std::exception const& ex) {
                    util::DebugTraceException(L"[DeviceEventRouter] Dispatch work ERROR", ex);
                } catch (...) {
                    util::DebugTraceUnknownException(L"[DeviceEventRouter] Dispatch work ERROR");
                }
            });
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceEventRouter] Dispatcher ERROR");
            return false;
        }
    } else {
        try {
            work();
        } catch (winrt::hresult_error const& ex) {
            util::DebugTraceException(L"[DeviceEventRouter] Dispatch inline ERROR", ex);
        } catch (std::exception const& ex) {
            util::DebugTraceException(L"[DeviceEventRouter] Dispatch inline ERROR", ex);
        } catch (...) {
            util::DebugTraceUnknownException(L"[DeviceEventRouter] Dispatch inline ERROR");
        }
        return true;
    }
}

void DeviceEventRouter::ResetTokens() noexcept {
    m_deviceConnectedToken = 0;
    m_deviceDisconnectedToken = 0;
    m_connectionErrorToken = 0;
    m_autoReconnectTriggeredToken = 0;
    m_autoReconnectFailedToken = 0;
    m_deviceStatusChangedToken = 0;
    m_deviceActivityChangedToken = 0;
    m_deviceInventoryChangedToken = 0;
}
