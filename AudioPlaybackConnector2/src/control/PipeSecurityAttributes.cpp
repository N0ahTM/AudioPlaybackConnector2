#include <control/PipeSecurityAttributes.hpp>

#include <sddl.h>

#include <cstddef>
#include <string>
#include <vector>

namespace apc::control {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Current User Access ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::optional<PipeSecurityAttributes> PipeSecurityAttributes::CreateCurrentUserOnly() {
    wil::unique_handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) return std::nullopt;

    DWORD required = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return std::nullopt;
    std::vector<std::byte> tokenBuffer(required);
    if (!GetTokenInformation(token.get(), TokenUser, tokenBuffer.data(), required, &required)) return std::nullopt;
    auto const* tokenUser = reinterpret_cast<TOKEN_USER const*>(tokenBuffer.data());

    wil::unique_hlocal_string sid;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, sid.put())) return std::nullopt;

    // GENERIC_READ | FILE_WRITE_DATA excludes FILE_CREATE_PIPE_INSTANCE.
    auto descriptor = std::wstring(L"D:P(A;;0x80000002;;;") + sid.get() + L")";
    PipeSecurityAttributes result;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor.c_str(), SDDL_REVISION_1, result.m_descriptor.put(), nullptr))
        return std::nullopt;
    return result;
}

SECURITY_ATTRIBUTES* PipeSecurityAttributes::Get() noexcept {
    if (!m_descriptor) return nullptr;
    m_attributes = {sizeof(SECURITY_ATTRIBUTES), m_descriptor.get(), FALSE};
    return &m_attributes;
}

} // namespace apc::control
