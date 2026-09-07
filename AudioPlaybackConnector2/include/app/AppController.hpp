#pragma once

#include <app/AppModels.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace apc::app {

// The controller is a transport- and UI-free boundary. The injected callbacks
// let the migration bridge retain legacy use-case ownership until Phase 4.
class AppController final {
    struct EventState;

public:
    using Executor = std::function<AppResult(AppCommand const&, AppCommandContext const&)>;
    using SnapshotProvider = std::function<AppSnapshot()>;
    using EventHandler = std::function<void(AppEvent const&)>;
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

    AppController(Executor executor, SnapshotProvider snapshotProvider);
    ~AppController() = default;

    AppController(AppController const&) = delete;
    AppController& operator=(AppController const&) = delete;
    AppController(AppController&&) = delete;
    AppController& operator=(AppController&&) = delete;

    [[nodiscard]] AppResult Execute(AppCommand command, AppCommandContext context = {}) const noexcept;
    [[nodiscard]] AppSnapshot Snapshot() const noexcept;

    // The bridge publishes normalized facts here; UI and CLI consume the one
    // typed subscription surface returned by Subscribe.
    [[nodiscard]] Subscription Subscribe(EventHandler handler);
    void Publish(AppEvent const& event) const noexcept;

private:
    Executor m_executor;
    SnapshotProvider m_snapshotProvider;
    std::shared_ptr<EventState> m_eventState;
};

} // namespace apc::app
