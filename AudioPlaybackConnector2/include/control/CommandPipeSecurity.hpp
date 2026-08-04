#pragma once

#include <windows.h>
#include <appmodel.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apc::control {

class PipeSecurityAttributes {
public:
    PipeSecurityAttributes() = default;
    ~PipeSecurityAttributes() {
        if (m_descriptor) LocalFree(m_descriptor);
    }

    PipeSecurityAttributes(PipeSecurityAttributes const&) = delete;
    PipeSecurityAttributes& operator=(PipeSecurityAttributes const&) = delete;

    PipeSecurityAttributes(PipeSecurityAttributes&& other) noexcept
        : m_descriptor(std::exchange(other.m_descriptor, nullptr)), m_attributes(other.m_attributes) {
        m_attributes.lpSecurityDescriptor = m_descriptor;
        other.m_attributes = {};
    }

    PipeSecurityAttributes& operator=(PipeSecurityAttributes&& other) noexcept {
        if (this != &other) {
            if (m_descriptor) LocalFree(m_descriptor);
            m_descriptor = std::exchange(other.m_descriptor, nullptr);
            m_attributes = other.m_attributes;
            m_attributes.lpSecurityDescriptor = m_descriptor;
            other.m_attributes = {};
        }
        return *this;
    }

    [[nodiscard]] static std::optional<PipeSecurityAttributes> CreateCurrentUserOnly() {
        PipeSecurityAttributes result;
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return std::nullopt;
        ScopedHandle token(rawToken);

        DWORD required = 0;
        GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &required);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return std::nullopt;

        std::vector<std::byte> tokenBuffer(required);
        if (!GetTokenInformation(token.Get(), TokenUser, tokenBuffer.data(), required, &required)) {
            return std::nullopt;
        }
        auto const* tokenUser = reinterpret_cast<TOKEN_USER const*>(tokenBuffer.data());

        LPWSTR rawSid = nullptr;
        if (!ConvertSidToStringSidW(tokenUser->User.Sid, &rawSid)) return std::nullopt;
        ScopedLocalMemory sid(rawSid);

        constexpr std::wstring_view clientAccess = L"0x80000002";
        const auto descriptor = std::wstring(L"D:P(A;;") + clientAccess.data() + L";;;" + rawSid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                descriptor.c_str(), SDDL_REVISION_1, &result.m_descriptor, nullptr)) {
            return std::nullopt;
        }

        result.m_attributes.nLength = sizeof(result.m_attributes);
        result.m_attributes.lpSecurityDescriptor = result.m_descriptor;
        result.m_attributes.bInheritHandle = FALSE;
        return result;
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* Get() noexcept { return m_descriptor ? &m_attributes : nullptr; }

private:
    class ScopedHandle {
    public:
        explicit ScopedHandle(HANDLE value) noexcept : m_value(value) {}
        ~ScopedHandle() {
            if (m_value) CloseHandle(m_value);
        }
        [[nodiscard]] HANDLE Get() const noexcept { return m_value; }

    private:
        HANDLE m_value = nullptr;
    };

    class ScopedLocalMemory {
    public:
        explicit ScopedLocalMemory(void* value) noexcept : m_value(value) {}
        ~ScopedLocalMemory() {
            if (m_value) LocalFree(m_value);
        }

    private:
        void* m_value = nullptr;
    };

    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES m_attributes{};
};

namespace details {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
    ~ScopedHandle() {
        if (m_value) CloseHandle(m_value);
    }
    ScopedHandle(ScopedHandle const&) = delete;
    ScopedHandle& operator=(ScopedHandle const&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_value != nullptr; }

private:
    HANDLE m_value = nullptr;
};

inline std::optional<std::vector<std::byte>> TokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD required = 0;
    GetTokenInformation(token, type, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return std::nullopt;

    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(token, type, buffer.data(), required, &required)) return std::nullopt;
    return buffer;
}

inline std::optional<std::wstring> ProcessImagePath(HANDLE process) {
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

inline std::optional<std::vector<std::byte>> CopySidBytes(PSID value) {
    if (!value || !IsValidSid(value)) return std::nullopt;
    const auto length = GetLengthSid(value);
    if (length == 0) return std::nullopt;
    std::vector<std::byte> sid(length);
    if (!CopySid(length, sid.data(), value)) return std::nullopt;
    return sid;
}

inline std::optional<std::vector<std::byte>> TokenUserSid(HANDLE token) {
    auto user = TokenInformation(token, TokenUser);
    if (!user) return std::nullopt;
    return CopySidBytes(reinterpret_cast<TOKEN_USER const*>(user->data())->User.Sid);
}

inline std::optional<std::vector<std::byte>> TokenLogonSid(HANDLE token) {
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

inline std::optional<DWORD> TokenSession(HANDLE token) {
    auto session = TokenInformation(token, TokenSessionId);
    if (!session || session->size() != sizeof(DWORD)) return std::nullopt;
    return *reinterpret_cast<DWORD const*>(session->data());
}

enum class PackageIdentityState { Unpackaged, Packaged, Failed };

struct PackageIdentity {
    PackageIdentityState State = PackageIdentityState::Failed;
    std::wstring FamilyName;
};

inline PackageIdentity ProcessPackageIdentity(HANDLE process) {
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

inline std::optional<ProcessIdentity> QueryProcessIdentity(HANDLE process, DWORD processId) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) return std::nullopt;
    ScopedHandle token(rawToken);

    auto userSid = TokenUserSid(token.Get());
    auto logonSid = TokenLogonSid(token.Get());
    auto tokenSessionId = TokenSession(token.Get());
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

inline ProcessIdentity const* CurrentProcessIdentity() noexcept {
    static std::mutex identityMutex;
    static std::optional<ProcessIdentity> identity;
    try {
        std::lock_guard lock(identityMutex);
        if (!identity) identity = QueryProcessIdentity(GetCurrentProcess(), GetCurrentProcessId());
        return identity ? &*identity : nullptr;
    } catch (...) {
        return nullptr;
    }
}

inline bool SameSid(std::vector<std::byte> const& lhs, std::vector<std::byte> const& rhs) noexcept {
    return !lhs.empty() && !rhs.empty() &&
           EqualSid(reinterpret_cast<PSID>(const_cast<std::byte*>(lhs.data())),
                    reinterpret_cast<PSID>(const_cast<std::byte*>(rhs.data()))) != FALSE;
}

inline bool IsSameTrustedIdentity(ProcessIdentity const& self, ProcessIdentity const& peer) noexcept {
    if (!SameSid(self.UserSid, peer.UserSid) || !SameSid(self.LogonSid, peer.LogonSid) ||
        self.TokenSessionId != peer.TokenSessionId || self.ProcessSessionId != peer.ProcessSessionId ||
        self.Package.State != peer.Package.State) {
        return false;
    }
    return self.Package.State == PackageIdentityState::Unpackaged || self.Package.FamilyName == peer.Package.FamilyName;
}

} // namespace details

struct ExecutableFileIdentity {
    ULONGLONG VolumeSerialNumber = 0;
    FILE_ID_128 FileId{};

    [[nodiscard]] bool operator==(ExecutableFileIdentity const& other) const noexcept {
        return VolumeSerialNumber == other.VolumeSerialNumber &&
               std::equal(FileId.Identifier, FileId.Identifier + sizeof(FileId.Identifier), other.FileId.Identifier);
    }
};

inline std::optional<ExecutableFileIdentity> ExecutableIdentityFromPath(std::wstring_view path) noexcept {
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
        details::ScopedHandle file(rawFile);
        FILE_ID_INFO info{};
        if (GetFileInformationByHandleEx(file.Get(), FileIdInfo, &info, sizeof(info))) {
            return ExecutableFileIdentity{info.VolumeSerialNumber, info.FileId};
        }

        // FileIdInfo is not implemented by every filesystem used for unpackaged development and test runs.
        const auto fileIdError = GetLastError();
        if (fileIdError != ERROR_INVALID_FUNCTION && fileIdError != ERROR_INVALID_PARAMETER &&
            fileIdError != ERROR_NOT_SUPPORTED) {
            return std::nullopt;
        }
        std::array<wchar_t, MAX_PATH + 1> fileSystemName{};
        if (!GetVolumeInformationByHandleW(file.Get(),
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
        if (!GetFileInformationByHandle(file.Get(), &legacy)) return std::nullopt;
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

inline std::optional<ExecutableFileIdentity> ProcessExecutableIdentity(HANDLE process) noexcept {
    try {
        auto path = details::ProcessImagePath(process);
        return path ? ExecutableIdentityFromPath(*path) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

inline bool IsTrustedPeerProcess(HANDLE process, DWORD processId) noexcept {
    try {
        if (!process || processId == 0) return false;
        auto const* self = details::CurrentProcessIdentity();
        if (!self) return false;
        auto peerIdentity = details::QueryProcessIdentity(process, processId);
        return peerIdentity && details::IsSameTrustedIdentity(*self, *peerIdentity);
    } catch (...) {
        return false;
    }
}

inline bool IsTrustedPeerProcess(HANDLE process,
                                 DWORD processId,
                                 std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept {
    try {
        if (!process || processId == 0) return false;
        auto const* self = details::CurrentProcessIdentity();
        if (!self) return false;
        auto peerIdentity = details::QueryProcessIdentity(process, processId);
        if (!peerIdentity || !details::IsSameTrustedIdentity(*self, *peerIdentity)) return false;
        if (self->Package.State != details::PackageIdentityState::Unpackaged) return true;
#if !defined(_DEBUG) && !defined(APC_ALLOW_UNPACKAGED_CONTROL)
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

inline bool IsTrustedPeerProcess(DWORD processId) noexcept {
    if (processId == 0) return false;
    details::ScopedHandle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    return peer && IsTrustedPeerProcess(peer.Get(), processId);
}

inline bool IsTrustedNamedPipeClient(HANDLE pipe) noexcept {
    ULONG processId = 0;
    return GetNamedPipeClientProcessId(pipe, &processId) && IsTrustedPeerProcess(processId);
}

inline bool IsTrustedNamedPipeClient(HANDLE pipe,
                                     std::optional<ExecutableFileIdentity> const& expectedUnpackagedIdentity) noexcept {
    ULONG processId = 0;
    if (!GetNamedPipeClientProcessId(pipe, &processId) || processId == 0) return false;
    details::ScopedHandle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    return peer && IsTrustedPeerProcess(peer.Get(), processId, expectedUnpackagedIdentity);
}

inline bool IsTrustedNamedPipeServer(HANDLE pipe) noexcept {
    ULONG processId = 0;
    return GetNamedPipeServerProcessId(pipe, &processId) && IsTrustedPeerProcess(processId);
}

} // namespace apc::control
