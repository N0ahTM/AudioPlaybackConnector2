#pragma once

#include <control/CommandClient.hpp>
#include <control/CommandPipeSecurity.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace apc::control::client {

class Win32CommandTransport final : public Transport {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Transport Operations //////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    Win32CommandTransport();
    AttemptResult TrySendOnce(Request const& request,
                              Response& response,
                              DWORD waitMs,
                              std::uint64_t overallDeadline,
                              ServerIdentityPtr& observedServer,
                              ServerIdentityPtr const& expectedServer) override;
    bool LaunchPackagedApp() override;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    const std::optional<std::wstring> m_pipeName;
    const std::optional<ExecutableFileIdentity> m_expectedUnpackagedServerIdentity;
};

} // namespace apc::control::client
