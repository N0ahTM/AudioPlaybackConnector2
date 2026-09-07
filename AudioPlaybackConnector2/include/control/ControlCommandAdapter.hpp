#pragma once

#include <app/AppController.hpp>
#include <control/CommandProtocol.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

namespace apc::control {

// Temporary Phase 1 boundary for the existing control transport. It owns
// protocol-to-intent translation and control presentation only; use-case
// execution remains in AppController and the transport remains in the pipe
// server/client.
class ControlCommandAdapter final {
public:
    using Localize = std::function<std::wstring(std::string_view)>;

    struct Options {
        Localize LocalizeResource;
    };

    ControlCommandAdapter(apc::app::AppController const& controller, Options options = {});

    ControlCommandAdapter(ControlCommandAdapter const&) = delete;
    ControlCommandAdapter& operator=(ControlCommandAdapter const&) = delete;

    [[nodiscard]] apc::control::Response
    Handle(apc::control::Request const& request, std::stop_token stopToken, std::uint64_t deadline) const noexcept;

private:
    apc::app::AppController const& m_controller;
    Options m_options;
    mutable std::mutex m_mutationMutex;
};

} // namespace apc::control
