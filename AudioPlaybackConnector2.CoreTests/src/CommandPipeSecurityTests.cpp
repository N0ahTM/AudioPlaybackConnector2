#include "TestCheck.hpp"

#include <control/CommandPipeSecurity.hpp>
#include <control/PipeSecurityAttributes.hpp>

#include <windows.h>
#include <wil/resource.h>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::wstring UniquePipeName(std::wstring_view testName) {
    static std::atomic_uint32_t sequence = 0;
    return L"\\\\.\\pipe\\AudioPlaybackConnector2.SecurityTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(++sequence) + L"." + std::wstring(testName);
}

void TestPipeSecurityAttributesRestrictCurrentUser() {
    auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
    Check(security && security->Get() && security->Get()->bInheritHandle == FALSE,
          "pipe security attributes must be non-inheritable");
    if (security && security->Get()) {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        Check(GetSecurityDescriptorControl(security->Get()->lpSecurityDescriptor, &control, &revision) != FALSE &&
                  (control & SE_DACL_PROTECTED) != 0,
              "pipe DACL must be protected from inherited broad ACEs");
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        Check(GetSecurityDescriptorDacl(security->Get()->lpSecurityDescriptor, &present, &dacl, &defaulted) != FALSE &&
                  present && dacl,
              "pipe security descriptor must contain a non-null DACL");
        if (dacl) {
            Check(dacl->AceCount == 1, "pipe DACL must contain only the current-user allow ACE");
            void* rawAce = nullptr;
            Check(dacl->AceCount == 1 && GetAce(dacl, 0, &rawAce) != FALSE && rawAce,
                  "pipe DACL must expose its only ACE");
            if (rawAce) {
                auto const* ace = static_cast<ACCESS_ALLOWED_ACE const*>(rawAce);
                Check(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE,
                      "pipe DACL must grant rather than deny the current user");
                constexpr ACCESS_MASK clientAccess = GENERIC_READ | FILE_WRITE_DATA;
                Check(ace->Mask == clientAccess, "pipe DACL must grant client I/O without FILE_CREATE_PIPE_INSTANCE");
                Check((ace->Mask & FILE_CREATE_PIPE_INSTANCE) == 0,
                      "pipe clients must not be able to create rogue server instances");
                wil::unique_handle token;
                Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) != FALSE,
                      "the DACL test must open the real current-process token");
                DWORD required = 0;
                GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
                std::vector<std::byte> userBuffer(required);
                auto const queried =
                    GetTokenInformation(token.get(), TokenUser, userBuffer.data(), required, &required);
                auto const* user = reinterpret_cast<TOKEN_USER const*>(userBuffer.data());
                auto* aceSid = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
                Check(queried && IsValidSid(aceSid) && EqualSid(user->User.Sid, aceSid) != FALSE,
                      "pipe DACL ACE must target the current user SID");
            }
        }

        const auto protectedName = UniquePipeName(L"protected-instance");
        wil::unique_handle protectedPipe(CreateNamedPipeW(protectedName.c_str(),
                                                          PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                          PIPE_UNLIMITED_INSTANCES,
                                                          1024,
                                                          1024,
                                                          0,
                                                          security->Get()));
        Check(static_cast<bool>(protectedPipe), "protected pipe fixture must be created");
        wil::unique_handle client(
            CreateFileW(protectedName.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        Check(static_cast<bool>(client), "the restricted DACL must still permit client read/write access");
        SetLastError(ERROR_SUCCESS);
        wil::unique_handle rogue(CreateNamedPipeW(protectedName.c_str(),
                                                  PIPE_ACCESS_DUPLEX,
                                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                  PIPE_UNLIMITED_INSTANCES,
                                                  1024,
                                                  1024,
                                                  0,
                                                  nullptr));
        Check(!rogue && GetLastError() == ERROR_ACCESS_DENIED,
              "the restricted DACL must reject same-user rogue server instances");
    }
}

void TestPeerTrustPredicates() {
    Check(apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId()),
          "an already-open current-process handle must trust itself without reopening the process");
    auto currentIdentity = apc::control::ProcessExecutableIdentity(GetCurrentProcess());
    Check(currentIdentity.has_value(), "the executable file identity must be available for the trust checks");
#if defined(_DEBUG)
    Check(apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId(), currentIdentity),
          "debug development trust must accept the exact current executable image");
#else
    Check(!apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId(), currentIdentity),
          "release trust must reject an unpackaged peer even with a matching executable identity");
#endif
    if (currentIdentity) {
        auto differentIdentity = *currentIdentity;
        differentIdentity.FileId.Identifier[0] ^= 1;
        Check(!apc::control::IsTrustedPeerProcess(
                  GetCurrentProcess(), GetCurrentProcessId(), std::optional(differentIdentity)),
              "strict unpackaged trust must reject a different executable file identity");
    }
    Check(!apc::control::IsTrustedPeerProcess(GetCurrentProcess(), GetCurrentProcessId(), std::nullopt),
          "strict unpackaged trust must reject a missing executable identity");
    Check(!apc::control::ExecutableIdentityFromPath(L"C:\\definitely-not-an-existing-executable.exe"),
          "invalid executable paths must fail closed without closing an invalid handle");
    Check(!apc::control::IsTrustedPeerProcess(GetCurrentProcess(), 0), "invalid peer identity must fail closed");
}

} // namespace

int RunCommandPipeSecurityTests() {
    TestPipeSecurityAttributesRestrictCurrentUser();
    TestPeerTrustPredicates();
    return g_failures;
}
