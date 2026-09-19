#pragma once

#include <app/AppController.hpp>
#include <control/CommandProtocol.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace apc::control {

// Validated wire requests call explicit AppController methods. Response formatting
// stays here; transport and peer authentication belong to the pipe server/client.
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

    // Formats a validated request's result without executing commands or reading owners.
    [[nodiscard]] static Response
    FormatResponse(Request const& request, apc::app::AppResult const& result, Options const& options);

private:
    apc::app::AppController const& m_controller;
    Options m_options;
    mutable std::mutex m_mutationMutex;
};

} // namespace apc::control
