#include <control/CommandPipeSecurity.hpp>

#include <appmodel.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace apc::control {
namespace {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Process Identity //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::optional<std::vector<std::byte>> TokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD required = 0;
    GetTokenInformation(token, type, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return std::nullopt;

    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(token, type, buffer.data(), required, &required)) return std::nullopt;
    return buffer;
}

std::optional<std::wstring> ProcessImagePath(HANDLE process) {
    DWORD capacity = 260;
    for (;;) {
        std::wstring path(capacity, L'\0');
        DWORD length = capacity;
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            if (length == 0) return std::nullopt;
            path.resize(length);
            return path;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || capacity >= 32'768) return std::nullopt;
        capacity = std::min<DWORD>(capacity * 2, 32'768);
    }
}

std::optional<std::vector<std::byte>> CopySidBytes(PSID value) {
    if (!value || !IsValidSid(value)) return std::nullopt;
    const auto length = GetLengthSid(value);
    if (length == 0) return std::nullopt;
    std::vector<std::byte> sid(length);
    if (!CopySid(length, sid.data(), value)) return std::nullopt;
    return sid;
}

std::optional<std::vector<std::byte>> TokenUserSid(HANDLE token) {
    auto user = TokenInformation(token, TokenUser);
    if (!user) return std::nullopt;
    return CopySidBytes(reinterpret_cast<TOKEN_USER const*>(user->data())->User.Sid);
}

std::optional<std::vector<std::byte>> TokenLogonSid(HANDLE token) {
    auto groups = TokenInformation(token, TokenGroups);
    if (!groups) return std::nullopt;
    auto const* tokenGroups = reinterpret_cast<TOKEN_GROUPS const*>(groups->data());
    for (DWORD index = 0; index < tokenGroups->GroupCount; ++index) {
        auto const& group = tokenGroups->Groups[index];
        if ((group.Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        return CopySidBytes(group.Sid);
    }
    return std::nullopt;
}

std::optional<DWORD> TokenSession(HANDLE token) {
    auto session = TokenInformation(token, TokenSessionId);
    if (!session || session->size() != sizeof(DWORD)) return std::nullopt;
    return *reinterpret_cast<DWORD const*>(session->data());
}

enum class PackageIdentityState { Unpackaged, Packaged, Failed };

struct PackageIdentity {
    PackageIdentityState State = PackageIdentityState::Failed;
    std::wstring FamilyName;
};

PackageIdentity ProcessPackageIdentity(HANDLE process) {
    UINT32 length = 0;
    const LONG initial = GetPackageFamilyName(process, &length, nullptr);
    if (initial == APPMODEL_ERROR_NO_PACKAGE) return {PackageIdentityState::Unpackaged, {}};
    if (initial != ERROR_INSUFFICIENT_BUFFER || length <= 1) return {};

    std::wstring familyName(length, L'\0');
    if (GetPackageFamilyName(process, &length, familyName.data()) != ERROR_SUCCESS || length <= 1) return {};
    familyName.resize(length - 1);
    return {PackageIdentityState::Packaged, std::move(familyName)};
}

struct ProcessIdentity {
    std::vector<std::byte> UserSid;
    std::vector<std::byte> LogonSid;
    DWORD TokenSessionId = 0;
    DWORD ProcessSessionId = 0;
    PackageIdentity Package;
};

std::optional<ProcessIdentity> QueryProcessIdentity(HANDLE process, DWORD processId) {
    wil::unique_handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, token.put())) return std::nullopt;

    auto userSid = TokenUserSid(token.get());
    auto logonSid = TokenLogonSid(token.get());
    auto tokenSessionId = TokenSession(token.get());
    DWORD processSessionId = 0;
    if (!userSid || !logonSid || !tokenSessionId || !ProcessIdToSessionId(processId, &processSessionId) ||
        processSessionId != *tokenSessionId) {
        return std::nullopt;
    }

    auto package = ProcessPackageIdentity(process);
    if (package.State == PackageIdentityState::Failed) return std::nullopt;
    return ProcessIdentity{
        std::move(*userSid), std::move(*logonSid), *tokenSessionId, processSessionId, std::move(package)};
}

bool SameSid(std::vector<std::byte> const& lhs, std::vector<std::byte> const& rhs) noexcept {
    return !lhs.empty() && !rhs.empty() &&
           EqualSid(reinterpret_cast<PSID>(const_cast<std::byte*>(lhs.data())),
                    reinterpret_cast<PSID>(const_cast<std::byte*>(rhs.data()))) != FALSE;
}

bool IsSameTrustedIdentity(ProcessIdentity const& self, ProcessIdentity const& peer) noexcept {
    if (!SameSid(self.UserSid, peer.UserSid) || !SameSid(self.LogonSid, peer.LogonSid) ||
        self.TokenSessionId != peer.TokenSessionId || self.ProcessSessionId != peer.ProcessSessionId ||
        self.Package.State != peer.Package.State) {
        return false;
    }
    return self.Package.State == PackageIdentityState::Unpackaged || self.Package.FamilyName == peer.Package.FamilyName;
}

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Executable Identity and Trust /////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

bool ExecutableFileIdentity::operator==(ExecutableFileIdentity const& other) const noexcept {
    return VolumeSerialNumber == other.VolumeSerialNumber &&
           std::equal(std::begin(FileId.Identifier), std::end(FileId.Identifier), std::begin(other.FileId.Identifier));
}

std::optional<ExecutableFileIdentity> ExecutableIdentityFromPath(std::wstring_view path) noexcept {
    try {
        if (path.empty()) return std::nullopt;
        std::wstring terminatedPath(path);
        const HANDLE rawFile = CreateFileW(terminatedPath.c_str(),
                                           FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
        if (rawFile == INVALID_HANDLE_VALUE) return std::nullopt;
        wil::unique_handle file(rawFile);
        FILE_ID_INFO info{};
        if (GetFileInformationByHandleEx(file.get(), FileIdInfo, &info, sizeof(info))) {
            return ExecutableFileIdentity{info.VolumeSerialNumber, info.FileId};
        }

        // FileIdInfo is not implemented by every filesystem used for unpackaged development and test runs.
        const auto fileIdError = GetLastError();
        if (fileIdError != ERROR_INVALID_FUNCTION && fileIdError != ERROR_INVALID_PARAMETER &&
            fileIdError != ERROR_NOT_SUPPORTED) {
            return std::nullopt;
        }
        std::array<wchar_t, MAX_PATH + 1> fileSystemName{};
        if (!GetVolumeInformationByHandleW(file.get(),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           fileSystemName.data(),
                                           static_cast<DWORD>(fileSystemName.size())) ||
            CompareStringOrdinal(fileSystemName.data(), -1, L"NTFS", -1, TRUE) != CSTR_EQUAL) {
            return std::nullopt;
        }
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (!GetFileInformationByHandle(file.get(), &legacy)) return std::nullopt;
        FILE_ID_128 legacyId{};
        const std::uint64_t fileIndex =
            (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
        if (fileIndex == 0 || legacy.dwVolumeSerialNumber == 0) return std::nullopt;
        std::memcpy(legacyId.Identifier, &fileIndex, sizeof(fileIndex));
        return ExecutableFileIdentity{legacy.dwVolumeSerialNumber, legacyId};
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ExecutableFileIdentity> ProcessExecutableIdentity(HANDLE process) noexcept {
    try {
        auto path = ProcessImagePath(process);
        return path ? ExecutableIdentityFromPath(*path) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool IsTrustedPeerProcess(HANDLE process, DWORD processId) noexcept {
    try {
        if (!process || processId == 0) return false;
        auto self = QueryProcessIdentity(GetCurrentProcess(), GetCurrentProcessId());
        if (!self) return false;
        auto peerIdentity = QueryProcessIdentity(process, processId);
        return peerIdentity && IsSameTrustedIdentity(*self, *peerIdentity);
    } catch (...) {
        return false;
    }
}

bool IsTrustedPeerProcess(HANDLE process,
                          DWORD processId,
                          std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept {
    try {
        if (!process || processId == 0) return false;
        auto self = QueryProcessIdentity(GetCurrentProcess(), GetCurrentProcessId());
        if (!self) return false;
        auto peerIdentity = QueryProcessIdentity(process, processId);
        if (!peerIdentity || !IsSameTrustedIdentity(*self, *peerIdentity)) return false;
        if (self->Package.State != PackageIdentityState::Unpackaged) return true;
#if !defined(_DEBUG)
        // Unpackaged equal-user processes have no OS-enforced code-identity boundary.
        (void)expectedUnpackagedIdentity;
        return false;
#else
        if (!expectedUnpackagedIdentity) return false;
        auto actualIdentity = ProcessExecutableIdentity(process);
        return actualIdentity && *actualIdentity == *expectedUnpackagedIdentity;
#endif
    } catch (...) {
        return false;
    }
}

bool IsTrustedNamedPipeClient(HANDLE pipe,
                              std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept {
    ULONG processId = 0;
    if (!GetNamedPipeClientProcessId(pipe, &processId) || processId == 0) return false;
    wil::unique_handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    return peer && IsTrustedPeerProcess(peer.get(), processId, expectedUnpackagedIdentity);
}

} // namespace apc::control
