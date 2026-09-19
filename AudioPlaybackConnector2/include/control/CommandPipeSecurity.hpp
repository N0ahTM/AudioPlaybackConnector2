#pragma once

#include <windows.h>

#include <optional>
#include <string_view>

namespace apc::control {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Executable Identity ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct ExecutableFileIdentity {
    ULONGLONG VolumeSerialNumber = 0;
    FILE_ID_128 FileId{};

    [[nodiscard]] bool operator==(ExecutableFileIdentity const& other) const noexcept;
};

[[nodiscard]] std::optional<ExecutableFileIdentity> ExecutableIdentityFromPath(std::wstring_view path) noexcept;
[[nodiscard]] std::optional<ExecutableFileIdentity> ProcessExecutableIdentity(HANDLE process) noexcept;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Peer Trust ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Matches user SID, logon SID, token/process sessions and package identity.
// Unpackaged peers need the stricter overload at the production command boundary.
[[nodiscard]] bool IsTrustedPeerProcess(HANDLE process, DWORD processId) noexcept;
// Release rejects unpackaged peers. Debug additionally requires an exact executable-file match.
[[nodiscard]] bool IsTrustedPeerProcess(
    HANDLE process, DWORD processId, std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept;
[[nodiscard]] bool IsTrustedPeerProcess(DWORD processId) noexcept;
[[nodiscard]] bool IsTrustedNamedPipeClient(HANDLE pipe) noexcept;
[[nodiscard]] bool
IsTrustedNamedPipeClient(HANDLE pipe, std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept;
[[nodiscard]] bool IsTrustedNamedPipeServer(HANDLE pipe) noexcept;

} // namespace apc::control
