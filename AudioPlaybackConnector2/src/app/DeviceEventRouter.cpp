#include <pch.h>

#include <app/DeviceEventRouter.hpp>
#include <app/DeviceFactPublicationFence.hpp>

#include <core/StringResources.hpp>

#include <mutex>
#include <unordered_map>

namespace {
using apc::device::DeviceConnectionResult;
using apc::device::DeviceLifecycleState;

struct DeviceStateHistory {
    DeviceLifecycleState State = DeviceLifecycleState::Idle;
    bool Seen = false;
};

[[nodiscard]] apc::device::DeviceSessionSnapshot const* FindSession(apc::device::DeviceServiceSnapshot const& snapshot,
                                                                    std::wstring_view deviceId) noexcept {
    for (auto const& session : snapshot.Sessions) {
        if (session.DeviceId == deviceId) return &session;
    }
    return nullptr;
}

[[nodiscard]] DeviceStatusKind ToStatusKind(DeviceLifecycleState state) noexcept {
    switch (state) {
        case DeviceLifecycleState::Idle: return DeviceStatusKind::Ready;
        case DeviceLifecycleState::Connecting:
        case DeviceLifecycleState::Disconnecting: return DeviceStatusKind::Connecting;
        case DeviceLifecycleState::Connected: return DeviceStatusKind::Connected;
        case DeviceLifecycleState::WaitingForReconnect: return DeviceStatusKind::Reconnecting;
        case DeviceLifecycleState::Failed: return DeviceStatusKind::Error;
    }
    return DeviceStatusKind::None;
}

[[nodiscard]] std::wstring StatusText(DeviceStatusKind status) {
    switch (status) {
        case DeviceStatusKind::Ready: return _("ReadyForConnection");
        case DeviceStatusKind::Connecting: return _("Connecting");
        case DeviceStatusKind::Reconnecting: return _("Reconnecting");
        case DeviceStatusKind::Connected: return _("Connected");
        case DeviceStatusKind::Error: return _("UnknownError");
        case DeviceStatusKind::None: break;
    }
    return {};
}

[[nodiscard]] std::wstring FailureText(DeviceConnectionResult result, bool isAutomaticReconnectTerminalFailure) {
    switch (result) {
        case DeviceConnectionResult::TimedOut: return _("RequestTimedOut");
        case DeviceConnectionResult::Denied: return _("DeniedBySystem");
        case DeviceConnectionResult::Success:
        case DeviceConnectionResult::Cancelled: return _("UnknownError");
        case DeviceConnectionResult::Failed:
            return isAutomaticReconnectTerminalFailure ? _("AutoReconnectFailed") : _("UnknownError");
    }
    return _("UnknownError");
}
} // namespace

struct DeviceEventRouter::State {
    std::atomic<bool> active = true;
    std::atomic<bool> deviceActivityDispatchPending = false;
    std::atomic<bool> deviceInventoryDispatchPending = false;
    apc::app::DeviceFactPublicationFence deviceFactPublicationFence;
    std::mutex historyMutex;
    std::unordered_map<std::wstring, DeviceStateHistory> history;
    UiDispatcher dispatcher;
    Callbacks callbacks;
};

DeviceEventRouter::~DeviceEventRouter() {
    Detach();
}

void DeviceEventRouter::Attach(std::shared_ptr<apc::device::DeviceService> deviceService,
                               UiDispatcher dispatcher,
                               Callbacks callbacks) {
    Detach();
    if (!deviceService) return;

    m_deviceService = std::move(deviceService);
    m_state = std::make_shared<State>();
    m_state->dispatcher = std::move(dispatcher);
    m_state->callbacks = std::move(callbacks);

    auto state = m_state;
    m_deviceFactSubscription = m_deviceService->Subscribe([state](apc::device::DeviceFact const& fact) {
        if (!state->active.load(std::memory_order_acquire)) return;

        if (fact.Kind == apc::device::DeviceFactKind::Shutdown) {
            state->active.store(false, std::memory_order_release);
            return;
        }

        auto dispatchActivity = [&] {
            if (state->deviceActivityDispatchPending.exchange(true, std::memory_order_acq_rel)) return;
            if (!DeviceEventRouter::Dispatch(state, [state] {
                    state->deviceActivityDispatchPending.store(false, std::memory_order_release);
                    if (!state->active.load(std::memory_order_acquire) || !state->callbacks.DeviceActivityChanged)
                        return;
                    state->callbacks.DeviceActivityChanged();
                })) {
                state->deviceActivityDispatchPending.store(false, std::memory_order_release);
            }
        };

        auto dispatchInventory = [&] {
            if (state->deviceInventoryDispatchPending.exchange(true, std::memory_order_acq_rel)) return;
            if (!DeviceEventRouter::Dispatch(state, [state] {
                    state->deviceInventoryDispatchPending.store(false, std::memory_order_release);
                    if (!state->active.load(std::memory_order_acquire) || !state->callbacks.DeviceInventoryChanged)
                        return;
                    state->callbacks.DeviceInventoryChanged();
                })) {
                state->deviceInventoryDispatchPending.store(false, std::memory_order_release);
            }
        };

        if (fact.Kind == apc::device::DeviceFactKind::InventoryChanged) {
            dispatchInventory();
            return;
        }

        if (fact.Kind != apc::device::DeviceFactKind::SessionChanged &&
            fact.Kind != apc::device::DeviceFactKind::OperationFailed) {
            return;
        }

        if (fact.DeviceId.empty()) {
            dispatchActivity();
            return;
        }

        auto const* session = FindSession(fact.Snapshot, fact.DeviceId);
        if (!session) {
            dispatchActivity();
            return;
        }

        DeviceStateHistory previous;
        {
            std::lock_guard lock(state->historyMutex);
            auto& history = state->history[fact.DeviceId];
            previous = history;
            history = {.State = session->State, .Seen = true};
        }

        const auto id = winrt::hstring(fact.DeviceId);
        const auto currentStatus = ToStatusKind(session->State);
        const auto statusText = winrt::hstring(StatusText(currentStatus));
        const auto isConnected = session->State == DeviceLifecycleState::Connected;
        const auto wasConnected = previous.Seen && previous.State == DeviceLifecycleState::Connected;

        if (!wasConnected && isConnected && state->callbacks.DeviceConnected) {
            const auto token = state->deviceFactPublicationFence.RecordConnected(fact.DeviceId);
            static_cast<void>(DeviceEventRouter::Dispatch(state, [state, id, token = std::move(token)] {
                if (!state->active.load(std::memory_order_acquire) ||
                    !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.DeviceConnected) {
                    return;
                }
                state->callbacks.DeviceConnected(id);
            }));
        } else if (wasConnected && !isConnected && state->callbacks.DeviceDisconnected) {
            const auto token = state->deviceFactPublicationFence.RecordDisconnected(fact.DeviceId);
            static_cast<void>(DeviceEventRouter::Dispatch(state, [state, id, token = std::move(token)] {
                if (!state->active.load(std::memory_order_acquire) ||
                    !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.DeviceDisconnected) {
                    return;
                }
                state->callbacks.DeviceDisconnected(id);
            }));
        }

        if (state->callbacks.DeviceStatusChanged && currentStatus != DeviceStatusKind::None) {
            const auto fenceStatus = [&] {
                using FenceStatus = apc::app::DeviceFactPublicationFence::Status;
                switch (currentStatus) {
                    case DeviceStatusKind::Ready: return FenceStatus::Ready;
                    case DeviceStatusKind::Connecting: return FenceStatus::Connecting;
                    case DeviceStatusKind::Reconnecting: return FenceStatus::Reconnecting;
                    case DeviceStatusKind::Connected: return FenceStatus::Connected;
                    case DeviceStatusKind::Error: return FenceStatus::Error;
                    case DeviceStatusKind::None: return FenceStatus::None;
                }
                return FenceStatus::None;
            }();
            const auto token = state->deviceFactPublicationFence.RecordStatus(fact.DeviceId, fenceStatus);
            static_cast<void>(
                DeviceEventRouter::Dispatch(state, [state, id, statusText, currentStatus, token = std::move(token)] {
                    if (!state->active.load(std::memory_order_acquire) ||
                        !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.DeviceStatusChanged) {
                        return;
                    }
                    state->callbacks.DeviceStatusChanged(id, statusText, currentStatus);
                }));
        }

        if (session->State == DeviceLifecycleState::WaitingForReconnect &&
            (!previous.Seen || previous.State != DeviceLifecycleState::WaitingForReconnect) &&
            state->callbacks.AutoReconnectTriggered) {
            const auto token = state->deviceFactPublicationFence.RecordStatus(
                fact.DeviceId, apc::app::DeviceFactPublicationFence::Status::WaitingForReconnect);
            static_cast<void>(DeviceEventRouter::Dispatch(state, [state, id, token = std::move(token)] {
                if (!state->active.load(std::memory_order_acquire) ||
                    !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.AutoReconnectTriggered) {
                    return;
                }
                state->callbacks.AutoReconnectTriggered(id);
            }));
        }

        if (fact.Kind == apc::device::DeviceFactKind::OperationFailed) {
            auto const isAutomaticReconnectTerminalFailure =
                fact.IsTerminalFailure && fact.Operation == apc::device::DeviceOperationKind::AutomaticReconnect;
            if (session->State == DeviceLifecycleState::Failed && state->callbacks.ConnectionError) {
                const auto message =
                    winrt::hstring(FailureText(fact.ConnectionResult, isAutomaticReconnectTerminalFailure));
                const auto token = state->deviceFactPublicationFence.RecordStatus(
                    fact.DeviceId, apc::app::DeviceFactPublicationFence::Status::Error);
                static_cast<void>(DeviceEventRouter::Dispatch(state, [state, id, message, token = std::move(token)] {
                    if (!state->active.load(std::memory_order_acquire) ||
                        !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.ConnectionError) {
                        return;
                    }
                    state->callbacks.ConnectionError(id, message);
                }));
            }
            if (isAutomaticReconnectTerminalFailure && state->callbacks.AutoReconnectFailed) {
                const auto token = state->deviceFactPublicationFence.RecordStatus(
                    fact.DeviceId, apc::app::DeviceFactPublicationFence::Status::Error);
                static_cast<void>(DeviceEventRouter::Dispatch(state, [state, id, token = std::move(token)] {
                    if (!state->active.load(std::memory_order_acquire) ||
                        !state->deviceFactPublicationFence.IsCurrent(token) || !state->callbacks.AutoReconnectFailed) {
                        return;
                    }
                    state->callbacks.AutoReconnectFailed(id);
                }));
            }
        }

        dispatchActivity();
    });
}

void DeviceEventRouter::Detach() noexcept {
    if (m_state) m_state->active.store(false, std::memory_order_release);

    auto deviceService = std::exchange(m_deviceService, nullptr);
    const auto subscription = std::exchange(m_deviceFactSubscription, 0);
    if (deviceService && subscription) {
        try {
            deviceService->Unsubscribe(subscription);
        } catch (...) {
            DebugTrace(L"[DeviceEventRouter] Detach ERROR: ignored exception");
        }
    }
    m_state.reset();
}

bool DeviceEventRouter::Dispatch(std::shared_ptr<State> const& state, std::function<void()> work) noexcept {
    if (!state || !state->active.load(std::memory_order_acquire)) return false;
    if (state->dispatcher) {
        try {
            return state->dispatcher([state, work = std::move(work)]() mutable {
                if (!state->active.load(std::memory_order_acquire)) return;
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
    }

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
