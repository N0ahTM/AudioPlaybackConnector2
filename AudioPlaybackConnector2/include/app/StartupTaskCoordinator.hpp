#pragma once

#include <app/StartupTaskSnapshot.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <winrt/Windows.Foundation.h>

class StartupTaskCoordinator final {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using QueryOperation = std::function<winrt::Windows::Foundation::IAsyncOperation<bool>()>;
    using SetOperation = std::function<winrt::Windows::Foundation::IAsyncOperation<bool>(bool)>;
    using CommitActual = std::function<void(bool)>;
    using ChangedHandler = std::function<void(StartupTaskSnapshot const&)>;
    using HandlerToken = std::uint64_t;

    explicit StartupTaskCoordinator(CommitActual commitActual);
    StartupTaskCoordinator(QueryOperation queryOperation, SetOperation setOperation, CommitActual commitActual);
    ~StartupTaskCoordinator();

    StartupTaskCoordinator(StartupTaskCoordinator const&) = delete;
    StartupTaskCoordinator& operator=(StartupTaskCoordinator const&) = delete;

    // Accepted requests join the serial owner context; completion is observed in Snapshot/Subscribe.
    bool Refresh() noexcept;
    bool RequestDesired(bool enabled) noexcept;
    [[nodiscard]] StartupTaskSnapshot Snapshot() const noexcept;
    [[nodiscard]] HandlerToken Subscribe(ChangedHandler handler);
    void Unsubscribe(HandlerToken token) noexcept;
    void Shutdown() noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    // Operations retain only State across WinRT suspension. Shutdown closes its queue and drains admitted
    // synchronous work; late OS completions cannot publish or persist. No coroutine retains this facade.
    struct State;
    std::shared_ptr<State> m_state;
};
