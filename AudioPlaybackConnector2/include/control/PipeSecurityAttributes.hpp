#pragma once

#include <windows.h>
#include <wil/resource.h>

#include <optional>

namespace apc::control {

class PipeSecurityAttributes final {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Construction //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    PipeSecurityAttributes() = default;
    PipeSecurityAttributes(PipeSecurityAttributes const&) = delete;
    PipeSecurityAttributes& operator=(PipeSecurityAttributes const&) = delete;
    PipeSecurityAttributes(PipeSecurityAttributes&&) noexcept = default;
    PipeSecurityAttributes& operator=(PipeSecurityAttributes&&) noexcept = default;

    [[nodiscard]] static std::optional<PipeSecurityAttributes> CreateCurrentUserOnly();
    [[nodiscard]] SECURITY_ATTRIBUTES* Get() noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    wil::unique_hlocal_security_descriptor m_descriptor;
    SECURITY_ATTRIBUTES m_attributes{};
};

} // namespace apc::control
