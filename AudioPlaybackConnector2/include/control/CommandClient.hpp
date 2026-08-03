#pragma once

#include <control/CommandProtocol.hpp>

#include <cstdint>
#include <memory>

namespace apc::control::client {

enum class AttemptResult { NotConnected, Rejected, ServerChanged, Indeterminate, Complete };
enum class SendResult { InvalidRequest, Unavailable, Indeterminate, Complete };

class ServerIdentity {
public:
    virtual ~ServerIdentity() = default;
};

using ServerIdentityPtr = std::shared_ptr<ServerIdentity const>;

class Transport {
public:
    virtual ~Transport() = default;
    virtual AttemptResult TrySendOnce(Request const& request,
                                      Response& response,
                                      DWORD waitMs,
                                      std::uint64_t overallDeadline,
                                      ServerIdentityPtr& observedServer,
                                      ServerIdentityPtr const& expectedServer) = 0;
    virtual bool LaunchPackagedApp() = 0;
};

[[nodiscard]] SendResult SendRequest(Transport& transport,
                                     Request const& request,
                                     Response& response,
                                     DWORD initialWaitMs = 250,
                                     DWORD launchWaitMs = 10000,
                                     DWORD overallTimeoutMs = 55000);

} // namespace apc::control::client
