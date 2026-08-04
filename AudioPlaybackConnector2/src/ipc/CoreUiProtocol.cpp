#include <ipc/CoreUiProtocol.hpp>

#include <bcrypt.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <control/CommandProtocol.hpp>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace apc::core_ui {
namespace {

static_assert(sizeof(wchar_t) == 2);

constexpr std::uint32_t c_knownSnapshotFlags = SnapshotFlagInventoryFresh | SnapshotFlagHasCachedInventory;
constexpr std::uint32_t c_knownDeviceFlags = DeviceFlagConnected | DeviceFlagBusy;
constexpr std::uint32_t c_knownKnownDeviceFlags =
    KnownDeviceFlagConnectOnStartup | KnownDeviceFlagReconnectOnConnectionLoss;
constexpr std::uint32_t c_knownSettingsFlags =
    SettingsFlagGlobalConnectOnStartup | SettingsFlagGlobalReconnectOnConnectionLoss |
    SettingsFlagAllowIncomingConnections | SettingsFlagStartWithWindowsRequested | SettingsFlagShowNotifications |
    SettingsFlagPrivacyMode | SettingsFlagSpecificDefaultDevice | SettingsFlagHasWindowBounds;
constexpr std::uint32_t c_knownAppearanceFlags = AppearanceFlagAllowMica | AppearanceFlagAllowAcrylic |
                                                 AppearanceFlagTransparencyEnabled | AppearanceFlagHighContrast |
                                                 AppearanceFlagRemoteSession | AppearanceFlagResourceConstrained;
constexpr std::uint32_t c_knownSurfaceFlags = SurfaceFlagHasAnchor | SurfaceFlagUserInitiated;
constexpr std::uint64_t c_snapshotDomains =
    CapabilityDevicePicker | CapabilitySettings | CapabilityAppearancePolicy | CapabilityUpdates;

class Encoder {
public:
    explicit Encoder(std::size_t limit = c_maxPayloadBytes, std::size_t initialCapacity = 0) : m_limit(limit) {
        m_bytes.reserve(std::min(limit, initialCapacity));
    }

    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    bool Append(T value) {
        using Unsigned = std::make_unsigned_t<T>;
        const auto raw = std::bit_cast<Unsigned>(value);
        if (sizeof(T) > m_limit || m_bytes.size() > m_limit - sizeof(T)) return false;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            m_bytes.push_back(static_cast<std::byte>((raw >> (index * 8)) & static_cast<Unsigned>(0xFF)));
        }
        return true;
    }

    bool Append(std::span<std::byte const> value) {
        if (value.size() > m_limit || m_bytes.size() > m_limit - value.size()) return false;
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return true;
    }

    bool AppendString(std::wstring_view value) {
        if (value.size() > c_maxStringCharacters || value.contains(L'\0') || !apc::limits::IsValidUtf16(value) ||
            value.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        if (!Append(static_cast<std::uint32_t>(value.size()))) return false;
        for (wchar_t character : value) {
            if (!Append(static_cast<std::uint16_t>(character))) return false;
        }
        return true;
    }

    [[nodiscard]] SecureByteBuffer Finish() && { return std::move(m_bytes); }

private:
    std::size_t m_limit;
    SecureByteBuffer m_bytes;
};

class Decoder {
public:
    explicit Decoder(std::span<std::byte const> bytes) noexcept : m_bytes(bytes) {}

    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    bool Read(T& value) noexcept {
        if (m_offset > m_bytes.size() || sizeof(T) > m_bytes.size() - m_offset) return false;
        using Unsigned = std::make_unsigned_t<T>;
        Unsigned raw = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            raw |= static_cast<Unsigned>(std::to_integer<unsigned int>(m_bytes[m_offset + index])) << (index * 8);
        }
        value = std::bit_cast<T>(raw);
        m_offset += sizeof(T);
        return true;
    }

    bool ReadBytes(std::span<std::byte> value) noexcept {
        if (m_offset > m_bytes.size() || value.size() > m_bytes.size() - m_offset) return false;
        if (!value.empty()) std::memcpy(value.data(), m_bytes.data() + m_offset, value.size());
        m_offset += value.size();
        return true;
    }

    bool ReadString(std::wstring& value) {
        std::uint32_t length = 0;
        if (!Read(length) || length > c_maxStringCharacters || length > (m_bytes.size() - m_offset) / 2) return false;
        value.resize(length);
        for (std::uint32_t index = 0; index < length; ++index) {
            std::uint16_t character = 0;
            if (!Read(character) || character == 0) return false;
            value[index] = static_cast<wchar_t>(character);
        }
        return apc::limits::IsValidUtf16(value);
    }

    [[nodiscard]] bool Finished() const noexcept { return m_offset == m_bytes.size(); }

private:
    std::span<std::byte const> m_bytes;
    std::size_t m_offset = 0;
};

[[nodiscard]] bool ConstantTimeEqual(std::span<std::byte const> lhs, std::span<std::byte const> rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        difference |= std::to_integer<std::uint8_t>(lhs[index] ^ rhs[index]);
    }
    return difference == 0;
}

class Sha256Provider {
public:
    Sha256Provider() noexcept {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            m_algorithm = nullptr;
        }
    }
    ~Sha256Provider() noexcept {
        if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    Sha256Provider(Sha256Provider const&) = delete;
    Sha256Provider& operator=(Sha256Provider const&) = delete;

    [[nodiscard]] std::optional<std::array<std::byte, 32>> Hash(std::span<std::byte const> value) noexcept {
        if (!m_algorithm) return std::nullopt;
        std::scoped_lock lock(m_mutex);
        std::array<std::byte, 32> digest{};
        const auto status = BCryptHash(m_algorithm,
                                       nullptr,
                                       0,
                                       reinterpret_cast<PUCHAR>(const_cast<std::byte*>(value.data())),
                                       static_cast<ULONG>(value.size()),
                                       reinterpret_cast<PUCHAR>(digest.data()),
                                       static_cast<ULONG>(digest.size()));
        return BCRYPT_SUCCESS(status) ? std::optional(digest) : std::nullopt;
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    std::mutex m_mutex;
};

[[nodiscard]] std::optional<std::array<std::byte, 32>> ComputeSha256(std::span<std::byte const> value) noexcept {
    static Sha256Provider provider;
    return provider.Hash(value);
}

[[nodiscard]] std::uint64_t FileTimeValue(FILETIME value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

[[nodiscard]] bool QueryCreationTime(HANDLE process, std::uint64_t& value) noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return false;
    }
    value = FileTimeValue(creation);
    return value != 0;
}

[[nodiscard]] bool IsProcessAlive(HANDLE process) noexcept {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

[[nodiscard]] bool
VerifyPipePeer(HANDLE pipe, HANDLE expectedProcess, ProcessBinding const& expected, bool client) noexcept {
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !expectedProcess || expected.ProcessId == 0 ||
        expected.CreationTime == 0 || !IsProcessAlive(expectedProcess) ||
        GetProcessId(expectedProcess) != expected.ProcessId) {
        return false;
    }
    ULONG actualProcessId = 0;
    const bool found = client ? GetNamedPipeClientProcessId(pipe, &actualProcessId) != FALSE
                              : GetNamedPipeServerProcessId(pipe, &actualProcessId) != FALSE;
    if (!found || actualProcessId != expected.ProcessId) return false;

    apc::control::details::ScopedHandle actualProcess(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, actualProcessId));
    std::uint64_t expectedHandleCreationTime = 0;
    std::uint64_t actualCreationTime = 0;
    if (!actualProcess || !IsProcessAlive(actualProcess.Get()) ||
        !QueryCreationTime(expectedProcess, expectedHandleCreationTime) ||
        !QueryCreationTime(actualProcess.Get(), actualCreationTime) ||
        expectedHandleCreationTime != expected.CreationTime || actualCreationTime != expected.CreationTime ||
        !apc::control::IsTrustedPeerProcess(actualProcess.Get(), expected.ProcessId)) {
        return false;
    }
    const auto actualPath = apc::control::details::ProcessImagePath(actualProcess.Get());
    if (expected.ImagePath.empty() || !actualPath ||
        CompareStringOrdinal(expected.ImagePath.c_str(),
                             static_cast<int>(expected.ImagePath.size()),
                             actualPath->c_str(),
                             static_cast<int>(actualPath->size()),
                             TRUE) != CSTR_EQUAL) {
        return false;
    }
    if (!expected.ExecutableIdentity) return false;
    const auto actualIdentity = apc::control::ProcessExecutableIdentity(actualProcess.Get());
    return actualIdentity && *actualIdentity == *expected.ExecutableIdentity;
}

[[nodiscard]] bool IsRequest(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::Subscribe:
        case MessageKind::RequestFullSnapshot:
        case MessageKind::SetDeviceConnectionState:
        case MessageKind::SetSetting:
        case MessageKind::AppActionRequest:
        case MessageKind::RefreshInventory: return true;
        default: return false;
    }
}

[[nodiscard]] bool IsResponse(MessageKind kind) noexcept {
    return kind == MessageKind::CommandResult;
}

[[nodiscard]] bool IsReplayProtectedRequest(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::SetDeviceConnectionState:
        case MessageKind::SetSetting:
        case MessageKind::RefreshInventory:
        case MessageKind::AppActionRequest: return true;
        default: return false;
    }
}

[[nodiscard]] bool IsClientMessage(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::Subscribe:
        case MessageKind::RequestFullSnapshot:
        case MessageKind::SetDeviceConnectionState:
        case MessageKind::SetSetting:
        case MessageKind::RefreshInventory:
        case MessageKind::SurfacePrepared:
        case MessageKind::SurfaceVisible:
        case MessageKind::SurfaceHidden:
        case MessageKind::SurfaceReleased:
        case MessageKind::SurfaceFailed:
        case MessageKind::AppliedRevision:
        case MessageKind::UiClosing:
        case MessageKind::AppActionRequest: return true;
        default: return false;
    }
}

[[nodiscard]] bool IsServerMessage(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::FullSnapshot:
        case MessageKind::StateChanged:
        case MessageKind::CommandResult:
        case MessageKind::ShowSurface:
        case MessageKind::CloseRequested: return true;
        default: return false;
    }
}

[[nodiscard]] bool IsFromClient(SessionRole role, FrameDirection direction) noexcept {
    return (role == SessionRole::Server && direction == FrameDirection::Inbound) ||
           (role == SessionRole::Client && direction == FrameDirection::Outbound);
}

[[nodiscard]] bool TryAdvanceSessionPhase(
    SessionRole role, FrameDirection direction, MessageKind kind, SessionPhase current, SessionPhase& next) noexcept {
    const bool fromClient = IsFromClient(role, direction);
    if (kind == MessageKind::ProtocolError) {
        next = SessionPhase::Closed;
        return current != SessionPhase::Closed;
    }
    switch (current) {
        case SessionPhase::Initial:
            if (!fromClient && kind == MessageKind::ServerChallenge) {
                next = SessionPhase::ChallengeExchanged;
                return true;
            }
            return false;
        case SessionPhase::ChallengeExchanged:
            if (fromClient && (kind == MessageKind::ClientAuthenticate || kind == MessageKind::ClientReconnect)) {
                next = SessionPhase::AuthenticationExchanged;
                return true;
            }
            return false;
        case SessionPhase::AuthenticationExchanged:
            if (!fromClient && kind == MessageKind::ServerAccepted) {
                next = SessionPhase::Active;
                return true;
            }
            return false;
        case SessionPhase::Active:
            if ((fromClient && !IsClientMessage(kind)) || (!fromClient && !IsServerMessage(kind))) return false;
            next = kind == MessageKind::UiClosing ? SessionPhase::Closed : SessionPhase::Active;
            return true;
        case SessionPhase::Closed: return false;
    }
    return false;
}

[[nodiscard]] bool IsExpectedChallenge(ServerChallenge const& actual, ServerChallenge const& expected) noexcept {
    return actual.Launch == expected.Launch &&
           (expected.ServerNonce.Empty() || actual.ServerNonce == expected.ServerNonce) &&
           actual.ServerProcessId == expected.ServerProcessId && actual.ClientProcessId == expected.ClientProcessId &&
           actual.ServerCreationTime == expected.ServerCreationTime &&
           actual.ClientCreationTime == expected.ClientCreationTime;
}

[[nodiscard]] bool IsHandshakeContextValid(SessionRole role, SessionHandshakeContext const& value) noexcept {
    const auto& challenge = value.Challenge;
    if (challenge.Launch.Empty() || challenge.ServerProcessId == 0 || challenge.ClientProcessId == 0 ||
        challenge.ServerCreationTime == 0 || challenge.ClientCreationTime == 0 || value.SupportedCapabilities == 0 ||
        (value.SupportedCapabilities & ~c_knownCapabilities) != 0 ||
        (role == SessionRole::Server) == challenge.ServerNonce.Empty()) {
        return false;
    }
    return !value.ReconnectCapability ||
           (!value.ReconnectCapability->Empty() && (value.SupportedCapabilities & CapabilityReconnect) != 0);
}

[[nodiscard]] bool IsKnownSettingKey(std::uint32_t value) noexcept {
    return value >= static_cast<std::uint32_t>(SettingKey::GlobalConnectOnStartup) &&
           value <= static_cast<std::uint32_t>(SettingKey::ForgetDevice);
}

[[nodiscard]] bool IsKnownValueKind(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(SettingValueKind::WindowBounds);
}

[[nodiscard]] bool IsKnownActionStatus(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(ActionStatus::Failed);
}

[[nodiscard]] bool IsKnownStartupTaskState(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(StartupTaskState::DisabledByPolicy);
}

[[nodiscard]] bool IsKnownUpdateStatus(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(UpdateStatus::Failed);
}

[[nodiscard]] bool IsKnownInventoryStatus(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(InventoryStatus::Cancelled);
}

[[nodiscard]] bool IsKnownDeviceAction(std::uint32_t value) noexcept {
    return value >= static_cast<std::uint32_t>(DeviceAction::SetConnected) &&
           value <= static_cast<std::uint32_t>(DeviceAction::ReconnectAll);
}

[[nodiscard]] bool IsKnownAppAction(std::uint32_t value) noexcept {
    return value >= static_cast<std::uint32_t>(AppAction::CheckForUpdates) &&
           value <= static_cast<std::uint32_t>(AppAction::OpenAvailableUpdate);
}

[[nodiscard]] bool IsValidBounds(WindowBounds const& value) noexcept {
    return value.Width > 0 && value.Height > 0 && value.Dpi >= apc::limits::c_minWindowDpi &&
           value.Dpi <= apc::limits::c_maxWindowDpi;
}

[[nodiscard]] bool IsMutationValid(SettingMutation const& value) noexcept {
    const bool hasDevice = !value.DeviceId.empty() && !value.DeviceId.contains(L'\0');
    if (value.DeviceId.size() > c_maxDeviceIdCharacters || value.StringValue.size() > c_maxStringCharacters ||
        value.StringValue.contains(L'\0')) {
        return false;
    }
    switch (value.Key) {
        case SettingKey::GlobalConnectOnStartup:
        case SettingKey::GlobalReconnectOnConnectionLoss:
        case SettingKey::AllowIncomingConnections:
        case SettingKey::StartWithWindows:
        case SettingKey::ShowNotifications:
        case SettingKey::PrivacyMode: return value.ValueKind == SettingValueKind::Boolean && !hasDevice;
        case SettingKey::Language:
            return value.ValueKind == SettingValueKind::String && !hasDevice &&
                   apc::limits::IsSupportedLanguage(value.StringValue);
        case SettingKey::SettingsWindowBounds:
            return !hasDevice &&
                   (value.ValueKind == SettingValueKind::None ||
                    (value.ValueKind == SettingValueKind::WindowBounds && IsValidBounds(value.BoundsValue)));
        case SettingKey::DeviceConnectOnStartup:
        case SettingKey::DeviceReconnectOnConnectionLoss:
            return value.ValueKind == SettingValueKind::Boolean && hasDevice;
        case SettingKey::DeviceAlias:
            return value.ValueKind == SettingValueKind::String && hasDevice &&
                   value.StringValue.size() <= c_maxDeviceAliasCharacters;
        case SettingKey::DefaultDevice:
            return value.ValueKind == SettingValueKind::String && !hasDevice &&
                   value.StringValue.size() <= c_maxDeviceIdCharacters;
        case SettingKey::ForgetDevice: return value.ValueKind == SettingValueKind::None && hasDevice;
    }
    return false;
}

[[nodiscard]] bool IsDeviceRequestValid(DeviceConnectionRequest const& value) noexcept {
    const bool hasDevice =
        !value.DeviceId.empty() && value.DeviceId.size() <= c_maxDeviceIdCharacters && !value.DeviceId.contains(L'\0');
    switch (value.Type) {
        case DeviceAction::SetConnected: return hasDevice;
        case DeviceAction::Reconnect: return hasDevice && !value.ShouldBeConnected;
        case DeviceAction::DisconnectAll: return !hasDevice && !value.ShouldBeConnected;
        case DeviceAction::ReconnectAll: return !hasDevice && !value.ShouldBeConnected;
    }
    return false;
}

[[nodiscard]] bool IsSurfaceInvocationValid(SurfaceInvocation const& value) noexcept {
    if ((value.Type != Surface::DevicePicker && value.Type != Surface::Settings) ||
        (value.Intent != SurfaceIntent::Prepare && value.Intent != SurfaceIntent::Show &&
         value.Intent != SurfaceIntent::Activate && value.Intent != SurfaceIntent::Hide &&
         value.Intent != SurfaceIntent::Toggle && value.Intent != SurfaceIntent::Release) ||
        value.Invocation.Empty() || (value.Flags & ~c_knownSurfaceFlags) != 0) {
        return false;
    }
    const bool hasAnchor = (value.Flags & SurfaceFlagHasAnchor) != 0;
    if (!hasAnchor) {
        return value.AnchorLeft == 0 && value.AnchorTop == 0 && value.AnchorRight == 0 && value.AnchorBottom == 0 &&
               value.Dpi == 0;
    }
    return value.AnchorRight > value.AnchorLeft && value.AnchorBottom > value.AnchorTop &&
           value.Dpi >= apc::limits::c_minWindowDpi && value.Dpi <= apc::limits::c_maxWindowDpi;
}

bool AppendRevisions(Encoder& encoder, StateRevisions const& value) {
    return encoder.Append(value.State) && encoder.Append(value.Inventory) && encoder.Append(value.Activity) &&
           encoder.Append(value.Settings) && encoder.Append(value.Presentation);
}

bool ReadRevisions(Decoder& decoder, StateRevisions& value) noexcept {
    return decoder.Read(value.State) && decoder.Read(value.Inventory) && decoder.Read(value.Activity) &&
           decoder.Read(value.Settings) && decoder.Read(value.Presentation);
}

[[nodiscard]] bool IsRevisionNoticeValid(RevisionNotice const& notice) noexcept {
    return notice.Revisions.State != 0 && notice.Domains != 0 && (notice.Domains & ~c_snapshotDomains) == 0 &&
           ((notice.Domains & CapabilityDevicePicker) != 0 ||
            (notice.Revisions.Inventory == 0 && notice.Revisions.Activity == 0)) &&
           ((notice.Domains & CapabilitySettings) != 0 || notice.Revisions.Settings == 0) &&
           ((notice.Domains & CapabilityAppearancePolicy) != 0 || notice.Revisions.Presentation == 0);
}

[[nodiscard]] bool IsSnapshotValid(Snapshot const& snapshot) {
    const auto validPage = [](std::uint32_t offset, std::uint32_t total, std::size_t count) noexcept {
        return total <= c_maxTotalDeviceCount && offset <= total && count <= total - offset &&
               (total == 0 ? offset == 0 && count == 0 : count != 0);
    };
    const bool inventoryFresh = (snapshot.Flags & SnapshotFlagInventoryFresh) != 0;
    const bool hasCachedInventory = (snapshot.Flags & SnapshotFlagHasCachedInventory) != 0;
    const bool inventoryStateValid =
        IsKnownInventoryStatus(static_cast<std::uint32_t>(snapshot.Inventory)) &&
        (snapshot.Inventory == InventoryStatus::Ready
             ? inventoryFresh && hasCachedInventory && snapshot.InventoryError == ERROR_SUCCESS
         : snapshot.Inventory == InventoryStatus::Failed ? !inventoryFresh && snapshot.InventoryError != ERROR_SUCCESS
         : snapshot.Inventory == InventoryStatus::Cancelled
             ? !inventoryFresh && snapshot.InventoryError == ERROR_CANCELLED
             : !inventoryFresh && snapshot.InventoryError == ERROR_SUCCESS);
    if (snapshot.Revisions.State == 0 || snapshot.Domains == 0 || (snapshot.Domains & ~c_snapshotDomains) != 0 ||
        snapshot.Devices.size() > c_maxDeviceCount || snapshot.KnownDevices.size() > c_maxDeviceCount ||
        !validPage(snapshot.KnownDeviceOffset, snapshot.KnownDeviceTotal, snapshot.KnownDevices.size()) ||
        !validPage(snapshot.DeviceOffset, snapshot.DeviceTotal, snapshot.Devices.size()) ||
        (snapshot.Flags & ~c_knownSnapshotFlags) != 0 || (snapshot.Appearance & ~c_knownAppearanceFlags) != 0 ||
        (snapshot.Settings.Flags & ~c_knownSettingsFlags) != 0 ||
        !IsKnownStartupTaskState(static_cast<std::uint32_t>(snapshot.Settings.StartupState)) ||
        !IsKnownUpdateStatus(static_cast<std::uint32_t>(snapshot.Update.Status)) ||
        snapshot.ConnectedDeviceCount > snapshot.DeviceTotal || !inventoryStateValid ||
        (!hasCachedInventory && (!snapshot.Devices.empty() || snapshot.ConnectedDeviceCount != 0)) ||
        ((snapshot.Settings.Flags & SettingsFlagHasWindowBounds) != 0 && !IsValidBounds(snapshot.Settings.Bounds)) ||
        !apc::limits::IsSupportedLanguage(snapshot.Settings.Language) ||
        snapshot.Settings.DefaultDeviceId.size() > c_maxDeviceIdCharacters ||
        snapshot.Update.AvailableVersion.size() > c_maxVersionCharacters ||
        snapshot.Settings.Language.contains(L'\0') || snapshot.Settings.DefaultDeviceId.contains(L'\0') ||
        snapshot.Update.AvailableVersion.contains(L'\0') ||
        (snapshot.Update.Status == UpdateStatus::Available) != !snapshot.Update.AvailableVersion.empty() ||
        ((snapshot.Settings.Flags & SettingsFlagSpecificDefaultDevice) != 0) !=
            !snapshot.Settings.DefaultDeviceId.empty()) {
        return false;
    }

    if ((snapshot.Domains & CapabilityDevicePicker) == 0 &&
        ((snapshot.Flags & (SnapshotFlagInventoryFresh | SnapshotFlagHasCachedInventory)) != 0 ||
         snapshot.Inventory != InventoryStatus::NotStarted || snapshot.InventoryError != ERROR_SUCCESS ||
         snapshot.ConnectedDeviceCount != 0 || snapshot.DeviceOffset != 0 || snapshot.DeviceTotal != 0 ||
         !snapshot.Devices.empty())) {
        return false;
    }
    if ((snapshot.Domains & CapabilitySettings) == 0 &&
        (snapshot.Settings != SettingsSnapshot{} || snapshot.KnownDeviceOffset != 0 || snapshot.KnownDeviceTotal != 0 ||
         !snapshot.KnownDevices.empty())) {
        return false;
    }
    if ((snapshot.Domains & CapabilityUpdates) == 0 && snapshot.Update != UpdateSnapshot{}) return false;
    if ((snapshot.Domains & CapabilityAppearancePolicy) == 0 && snapshot.Appearance != AppearanceFlagNone) return false;

    if ((snapshot.Settings.Flags & SettingsFlagHasWindowBounds) == 0 &&
        (snapshot.Settings.Bounds.X != 0 || snapshot.Settings.Bounds.Y != 0 || snapshot.Settings.Bounds.Width != 0 ||
         snapshot.Settings.Bounds.Height != 0 || snapshot.Settings.Bounds.Dpi != USER_DEFAULT_SCREEN_DPI)) {
        return false;
    }

    std::unordered_set<std::wstring_view> knownIds;
    knownIds.reserve(snapshot.KnownDevices.size());
    for (auto const& device : snapshot.KnownDevices) {
        if (device.Id.empty() || device.Id.size() > c_maxDeviceIdCharacters ||
            device.Name.size() > c_maxDeviceNameCharacters || device.Alias.size() > c_maxDeviceAliasCharacters ||
            device.Id.contains(L'\0') || device.Name.contains(L'\0') || device.Alias.contains(L'\0') ||
            (device.Flags & ~c_knownKnownDeviceFlags) != 0 || !knownIds.insert(device.Id).second) {
            return false;
        }
    }

    std::unordered_set<std::wstring_view> ids;
    ids.reserve(snapshot.Devices.size());
    std::uint32_t connected = 0;
    for (auto const& device : snapshot.Devices) {
        if (device.Id.empty() || device.Id.size() > c_maxDeviceIdCharacters ||
            device.Name.size() > c_maxDeviceNameCharacters || device.Alias.size() > c_maxDeviceAliasCharacters ||
            device.DisplayName.size() > c_maxDeviceNameCharacters || device.Id.contains(L'\0') ||
            device.Name.contains(L'\0') || device.Alias.contains(L'\0') || device.DisplayName.contains(L'\0') ||
            (device.Flags & ~c_knownDeviceFlags) != 0 || !ids.insert(device.Id).second) {
            return false;
        }
        if ((device.Flags & DeviceFlagConnected) != 0) ++connected;
    }
    const bool completeDevicePage = snapshot.DeviceOffset == 0 && snapshot.Devices.size() == snapshot.DeviceTotal;
    return connected <= snapshot.ConnectedDeviceCount &&
           (!completeDevicePage || connected == snapshot.ConnectedDeviceCount);
}

[[nodiscard]] IoStatus MapIoStatus(apc::control::IoStatus status) noexcept {
    switch (status) {
        case apc::control::IoStatus::Success: return IoStatus::Success;
        case apc::control::IoStatus::Timeout: return IoStatus::Timeout;
        case apc::control::IoStatus::Cancelled: return IoStatus::Cancelled;
        case apc::control::IoStatus::Closed: return IoStatus::Closed;
        case apc::control::IoStatus::InvalidData: return IoStatus::InvalidData;
        case apc::control::IoStatus::Failed: return IoStatus::Failed;
    }
    return IoStatus::Failed;
}

[[nodiscard]] std::optional<std::uint32_t> DecodeFrameHeader(std::span<std::byte const> bytes, Frame& frame) noexcept {
    if (bytes.size() != c_wireHeaderBytes) return std::nullopt;
    Decoder decoder(bytes);
    std::uint32_t magic = 0;
    std::uint16_t major = 0, minor = 0, headerBytes = 0, reserved = 0;
    std::uint32_t kind = 0, payloadBytes = 0;
    if (!decoder.Read(magic) || !decoder.Read(major) || !decoder.Read(minor) || !decoder.Read(headerBytes) ||
        !decoder.Read(reserved) || !decoder.Read(kind) || !decoder.Read(frame.Flags) || !decoder.Read(payloadBytes) ||
        !decoder.ReadBytes(frame.Epoch.Bytes) || !decoder.ReadBytes(frame.Connection.Bytes) ||
        !decoder.Read(frame.Sequence) || !decoder.ReadBytes(frame.Correlation.Bytes) || !decoder.Finished()) {
        return std::nullopt;
    }
    const auto messageKind = static_cast<MessageKind>(kind);
    if (magic != c_frameMagic || major != c_protocolMajor || minor != c_protocolMinor ||
        headerBytes != c_wireHeaderBytes || reserved != 0 || !IsKnownMessageKind(kind) || frame.Flags != 0 ||
        payloadBytes > c_maxPayloadBytes || frame.Epoch.Empty() || frame.Connection.Empty() || frame.Sequence == 0 ||
        (IsRequest(messageKind) || IsResponse(messageKind)) != !frame.Correlation.Empty()) {
        return std::nullopt;
    }
    frame.Kind = messageKind;
    return payloadBytes;
}

[[nodiscard]] bool IsFrameEnvelopeValid(Frame const& frame) noexcept;
[[nodiscard]] bool IsFramePayloadValid(Frame const& frame) noexcept;
[[nodiscard]] bool IsActiveFrameAllowed(Frame const& frame, std::uint64_t capabilities) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeFrameEnvelope(Frame const& frame);
[[nodiscard]] std::optional<Frame> DecodeFrameEnvelope(std::span<std::byte const> wireBytes) noexcept;

} // namespace

bool Identifier128::Empty() const noexcept {
    std::byte combined{};
    for (auto value : Bytes)
        combined |= value;
    return combined == std::byte{};
}

bool operator==(Identifier128 const& lhs, Identifier128 const& rhs) noexcept {
    return ConstantTimeEqual(lhs.Bytes, rhs.Bytes);
}

bool Secret256::Empty() const noexcept {
    std::byte combined{};
    for (auto value : Bytes)
        combined |= value;
    return combined == std::byte{};
}

Secret256::Secret256(Secret256&& other) noexcept : Bytes(other.Bytes) {
    other.Clear();
}

Secret256& Secret256::operator=(Secret256&& other) noexcept {
    if (this != &other) {
        Clear();
        Bytes = other.Bytes;
        other.Clear();
    }
    return *this;
}

Secret256::~Secret256() noexcept {
    Clear();
}

void Secret256::Clear() noexcept {
    SecureZeroMemory(Bytes.data(), Bytes.size());
}

bool operator==(Secret256 const& lhs, Secret256 const& rhs) noexcept {
    return ConstantTimeEqual(lhs.Bytes, rhs.Bytes);
}

bool SequenceValidator::Accept(std::uint64_t sequence) noexcept {
    if (sequence == 0 || sequence != m_lastAccepted + 1) return false;
    m_lastAccepted = sequence;
    return true;
}

void SequenceValidator::Reset() noexcept {
    m_lastAccepted = 0;
}

SessionStateMachine::SessionStateMachine(SessionRole role,
                                         HostEpoch epoch,
                                         ConnectionId connection,
                                         SessionHandshakeContext handshake) noexcept
    : m_role(role), m_epoch(epoch), m_connection(connection), m_handshake(std::move(handshake)) {
    if (m_epoch.Empty() || m_connection.Empty() || !IsHandshakeContextValid(m_role, m_handshake)) {
        m_phase = SessionPhase::Closed;
        ClearSecretsLocked();
    }
}

SessionStateMachine::~SessionStateMachine() noexcept {
    Close();
}

bool SessionStateMachine::ValidateAndAdvance(Frame const& frame, FrameDirection direction) noexcept {
    return ValidateAndAdvanceCore(frame, direction);
}

bool SessionStateMachine::ValidateAndAdvanceCore(Frame const& frame, FrameDirection direction) noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        if (m_phase == SessionPhase::Closed || !IsFrameEnvelopeValid(frame) || !(frame.Epoch == m_epoch) ||
            !(frame.Connection == m_connection)) {
            return false;
        }

        auto& sequence = direction == FrameDirection::Inbound ? m_lastInboundSequence : m_lastOutboundSequence;
        if (sequence == std::numeric_limits<std::uint64_t>::max() || frame.Sequence != sequence + 1) return false;

        auto next = m_phase;
        if (!TryAdvanceSessionPhase(m_role, direction, frame.Kind, m_phase, next)) return false;

        if (frame.Kind == MessageKind::ProtocolError) {
            if (!IsFramePayloadValid(frame)) return false;
            sequence = frame.Sequence;
            m_phase = SessionPhase::Closed;
            ClearSecretsLocked();
            return true;
        }

        const auto previous = m_phase;
        switch (previous) {
            case SessionPhase::Initial: {
                auto challenge = DecodeServerChallenge(frame.Payload);
                if (!challenge || !IsExpectedChallenge(*challenge, m_handshake.Challenge)) return false;
                if (m_role == SessionRole::Client) m_handshake.Challenge.ServerNonce = challenge->ServerNonce;
                break;
            }
            case SessionPhase::ChallengeExchanged:
                if (frame.Kind == MessageKind::ClientAuthenticate) {
                    if (m_handshake.ReconnectCapability) return false;
                    auto authentication = DecodeClientAuthenticate(frame.Payload);
                    if (!authentication || !(authentication->Launch == m_handshake.Challenge.Launch) ||
                        !(authentication->ServerNonce == m_handshake.Challenge.ServerNonce)) {
                        return false;
                    }
                    m_clientNonce = authentication->ClientNonce;
                    m_offeredCapabilities = authentication->Capabilities;
                } else {
                    if (!m_handshake.ReconnectCapability) return false;
                    auto authentication = DecodeReconnectAuthenticate(frame.Payload);
                    if (!authentication || !(authentication->Launch == m_handshake.Challenge.Launch) ||
                        !(authentication->ServerNonce == m_handshake.Challenge.ServerNonce) ||
                        (authentication->Capabilities & CapabilityReconnect) == 0) {
                        return false;
                    }
                    auto proof = ComputeReconnectProof(*m_handshake.ReconnectCapability,
                                                       m_epoch,
                                                       m_handshake.Challenge.Launch,
                                                       m_connection,
                                                       authentication->ServerNonce,
                                                       authentication->ClientNonce,
                                                       m_handshake.Challenge.ServerProcessId,
                                                       m_handshake.Challenge.ClientProcessId,
                                                       authentication->Capabilities);
                    if (!proof || !(*proof == authentication->Proof)) return false;
                    m_clientNonce = authentication->ClientNonce;
                    m_offeredCapabilities = authentication->Capabilities;
                }
                break;
            case SessionPhase::AuthenticationExchanged: {
                auto accepted = DecodeServerAccepted(frame.Payload);
                const auto negotiated = NegotiateCapabilities(m_offeredCapabilities, m_handshake.SupportedCapabilities);
                if (!accepted || negotiated == 0 || accepted->Capabilities != negotiated ||
                    (m_handshake.ReconnectCapability &&
                     accepted->ReconnectCapability == *m_handshake.ReconnectCapability)) {
                    return false;
                }
                m_negotiatedCapabilities = negotiated;
                if (accepted->ReconnectCapability.Empty()) {
                    if (m_establishedCapability) m_establishedCapability->Clear();
                    m_establishedCapability.reset();
                } else {
                    m_establishedCapability = accepted->ReconnectCapability;
                }
                break;
            }
            case SessionPhase::Active:
                if (!IsActiveFrameAllowed(frame, m_negotiatedCapabilities)) return false;
                break;
            case SessionPhase::Closed: return false;
        }

        sequence = frame.Sequence;
        m_phase = next;
        if (previous == SessionPhase::AuthenticationExchanged && m_phase == SessionPhase::Active) {
            m_clientNonce.Clear();
            m_handshake.Challenge.ServerNonce.Clear();
            if (m_handshake.ReconnectCapability) {
                m_handshake.ReconnectCapability->Clear();
                m_handshake.ReconnectCapability.reset();
            }
            m_offeredCapabilities = 0;
        } else if (m_phase == SessionPhase::Closed) {
            ClearSecretsLocked();
        }
        return true;
    } catch (...) {
        return false;
    }
}

void SessionStateMachine::ClearSecretsLocked() noexcept {
    m_clientNonce.Clear();
    m_handshake.Challenge.ServerNonce.Clear();
    if (m_handshake.ReconnectCapability) {
        m_handshake.ReconnectCapability->Clear();
        m_handshake.ReconnectCapability.reset();
    }
    if (m_establishedCapability) {
        m_establishedCapability->Clear();
        m_establishedCapability.reset();
    }
    m_offeredCapabilities = 0;
    m_negotiatedCapabilities = 0;
}

void SessionStateMachine::Close() noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        m_phase = SessionPhase::Closed;
        ClearSecretsLocked();
    } catch (...) {
    }
}

std::uint64_t SessionStateMachine::NegotiatedCapabilities() const noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        return m_negotiatedCapabilities;
    } catch (...) {
        return 0;
    }
}

std::optional<SessionCapability> SessionStateMachine::EstablishedCapability() const noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        return m_establishedCapability;
    } catch (...) {
        return std::nullopt;
    }
}

SessionPhase SessionStateMachine::Phase() const noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        return m_phase;
    } catch (...) {
        return SessionPhase::Closed;
    }
}

std::optional<Identifier128> GenerateIdentifier() noexcept {
    Identifier128 value;
    const auto status = BCryptGenRandom(nullptr,
                                        reinterpret_cast<PUCHAR>(value.Bytes.data()),
                                        static_cast<ULONG>(value.Bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status) && !value.Empty() ? std::optional(value) : std::nullopt;
}

std::optional<Secret256> GenerateSecret() noexcept {
    Secret256 value;
    const auto status = BCryptGenRandom(nullptr,
                                        reinterpret_cast<PUCHAR>(value.Bytes.data()),
                                        static_cast<ULONG>(value.Bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status) && !value.Empty() ? std::optional(value) : std::nullopt;
}

std::uint64_t NegotiateCapabilities(std::uint64_t clientCapabilities, std::uint64_t serverCapabilities) noexcept {
    if ((clientCapabilities & ~c_knownCapabilities) != 0 || (serverCapabilities & ~c_knownCapabilities) != 0) return 0;
    return clientCapabilities & serverCapabilities & c_knownCapabilities;
}

std::optional<std::wstring> PipeName(LaunchId const& launch) noexcept {
    try {
        if (launch.Empty()) return std::nullopt;
        DWORD sessionId = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return std::nullopt;
        constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring suffix;
        suffix.reserve(launch.Bytes.size() * 2);
        for (auto value : launch.Bytes) {
            const auto byte = std::to_integer<unsigned int>(value);
            suffix.push_back(digits[(byte >> 4) & 0xF]);
            suffix.push_back(digits[byte & 0xF]);
        }
        return std::wstring(c_pipeNamePrefix) + std::to_wstring(sessionId) + L"." + suffix;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::wstring> EncodeUiLaunchBootstrap(UiLaunchBootstrap const& bootstrap) noexcept {
    try {
        if (bootstrap.Launch.Empty() || bootstrap.Epoch.Empty() || bootstrap.Connection.Empty() ||
            bootstrap.InheritedCoreProcessHandle == 0 ||
            bootstrap.InheritedCoreProcessHandle == reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE) ||
            bootstrap.CoreProcessId == 0 || bootstrap.CoreCreationTime == 0 || bootstrap.SupportedCapabilities == 0 ||
            (bootstrap.SupportedCapabilities & ~c_knownCapabilities) != 0) {
            return std::nullopt;
        }
        Encoder encoder(128);
        if (!encoder.Append(bootstrap.Launch.Bytes) || !encoder.Append(bootstrap.Epoch.Bytes) ||
            !encoder.Append(bootstrap.Connection.Bytes) ||
            !encoder.Append(static_cast<std::uint64_t>(bootstrap.InheritedCoreProcessHandle)) ||
            !encoder.Append(bootstrap.CoreProcessId) || !encoder.Append(bootstrap.CoreCreationTime) ||
            !encoder.Append(bootstrap.SupportedCapabilities)) {
            return std::nullopt;
        }
        auto bytes = std::move(encoder).Finish();
        constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring token = L"apcui1:";
        token.reserve(token.size() + bytes.size() * 2);
        for (auto value : bytes) {
            const auto byte = std::to_integer<unsigned int>(value);
            token.push_back(digits[(byte >> 4) & 0xF]);
            token.push_back(digits[byte & 0xF]);
        }
        return token;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<UiLaunchBootstrap> DecodeUiLaunchBootstrap(std::wstring_view token) noexcept {
    try {
        constexpr std::wstring_view prefix = L"apcui1:";
        constexpr std::size_t payloadBytes = 16 + 16 + 16 + 8 + 4 + 8 + 8;
        if (!token.starts_with(prefix) || token.size() != prefix.size() + payloadBytes * 2) return std::nullopt;
        auto hexValue = [](wchar_t value) noexcept -> std::optional<std::byte> {
            if (value >= L'0' && value <= L'9') return static_cast<std::byte>(value - L'0');
            if (value >= L'a' && value <= L'f') return static_cast<std::byte>(value - L'a' + 10);
            if (value >= L'A' && value <= L'F') return static_cast<std::byte>(value - L'A' + 10);
            return std::nullopt;
        };
        SecureByteBuffer bytes;
        bytes.reserve(payloadBytes);
        for (std::size_t index = prefix.size(); index < token.size(); index += 2) {
            auto high = hexValue(token[index]);
            auto low = hexValue(token[index + 1]);
            if (!high || !low) return std::nullopt;
            bytes.push_back(static_cast<std::byte>((std::to_integer<unsigned int>(*high) << 4) |
                                                   std::to_integer<unsigned int>(*low)));
        }
        Decoder decoder(bytes);
        UiLaunchBootstrap bootstrap;
        std::uint64_t inheritedHandle = 0;
        if (!decoder.ReadBytes(bootstrap.Launch.Bytes) || !decoder.ReadBytes(bootstrap.Epoch.Bytes) ||
            !decoder.ReadBytes(bootstrap.Connection.Bytes) || !decoder.Read(inheritedHandle) ||
            inheritedHandle > std::numeric_limits<std::uintptr_t>::max() || !decoder.Read(bootstrap.CoreProcessId) ||
            !decoder.Read(bootstrap.CoreCreationTime) || !decoder.Read(bootstrap.SupportedCapabilities) ||
            !decoder.Finished()) {
            return std::nullopt;
        }
        bootstrap.InheritedCoreProcessHandle = static_cast<std::uintptr_t>(inheritedHandle);
        return EncodeUiLaunchBootstrap(bootstrap) ? std::optional(bootstrap) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ProcessBinding> CaptureProcessBinding(HANDLE process, std::wstring_view executablePath) noexcept {
    try {
        if (!process) return std::nullopt;
        ProcessBinding result;
        result.ProcessId = GetProcessId(process);
        if (result.ProcessId == 0 || !QueryCreationTime(process, result.CreationTime)) return std::nullopt;
        auto actualPath = apc::control::details::ProcessImagePath(process);
        if (!actualPath && result.ProcessId == GetCurrentProcessId()) {
            std::wstring modulePath(32'768, L'\0');
            const auto length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
            if (length > 0 && length < modulePath.size()) {
                modulePath.resize(length);
                actualPath = std::move(modulePath);
            }
        }
        if (!actualPath || actualPath->empty()) return std::nullopt;
        result.ImagePath = std::move(*actualPath);
        if (!executablePath.empty() && CompareStringOrdinal(result.ImagePath.c_str(),
                                                            static_cast<int>(result.ImagePath.size()),
                                                            executablePath.data(),
                                                            static_cast<int>(executablePath.size()),
                                                            TRUE) != CSTR_EQUAL) {
            return std::nullopt;
        }
        result.ExecutableIdentity = executablePath.empty() ? apc::control::ProcessExecutableIdentity(process)
                                                           : apc::control::ExecutableIdentityFromPath(executablePath);
        return result.ExecutableIdentity ? std::optional(std::move(result)) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool IsExpectedTrustedPipeClient(HANDLE pipe, HANDLE expectedProcess, ProcessBinding const& expected) noexcept {
    return VerifyPipePeer(pipe, expectedProcess, expected, true);
}

bool IsExpectedTrustedPipeServer(HANDLE pipe, HANDLE expectedProcess, ProcessBinding const& expected) noexcept {
    return VerifyPipePeer(pipe, expectedProcess, expected, false);
}

bool IsKnownMessageKind(std::uint32_t value) noexcept {
    switch (static_cast<MessageKind>(value)) {
        case MessageKind::ServerChallenge:
        case MessageKind::ClientAuthenticate:
        case MessageKind::ServerAccepted:
        case MessageKind::ClientReconnect:
        case MessageKind::Subscribe:
        case MessageKind::RequestFullSnapshot:
        case MessageKind::SetDeviceConnectionState:
        case MessageKind::SetSetting:
        case MessageKind::RefreshInventory:
        case MessageKind::SurfacePrepared:
        case MessageKind::SurfaceVisible:
        case MessageKind::SurfaceHidden:
        case MessageKind::SurfaceReleased:
        case MessageKind::SurfaceFailed:
        case MessageKind::AppliedRevision:
        case MessageKind::UiClosing:
        case MessageKind::AppActionRequest:
        case MessageKind::FullSnapshot:
        case MessageKind::StateChanged:
        case MessageKind::CommandResult:
        case MessageKind::ShowSurface:
        case MessageKind::CloseRequested:
        case MessageKind::ProtocolError: return true;
    }
    return false;
}

namespace {

bool IsFrameEnvelopeValid(Frame const& frame) noexcept {
    if (!IsKnownMessageKind(static_cast<std::uint32_t>(frame.Kind)) || frame.Flags != 0 || frame.Epoch.Empty() ||
        frame.Connection.Empty() || frame.Sequence == 0 || frame.Payload.size() > c_maxPayloadBytes ||
        (IsRequest(frame.Kind) || IsResponse(frame.Kind)) != !frame.Correlation.Empty()) {
        return false;
    }
    return true;
}

bool IsFramePayloadValid(Frame const& frame) noexcept {
    switch (frame.Kind) {
        case MessageKind::Subscribe:
        case MessageKind::RequestFullSnapshot:
        case MessageKind::RefreshInventory:
        case MessageKind::UiClosing:
        case MessageKind::CloseRequested: return frame.Payload.empty();
        case MessageKind::SurfacePrepared:
        case MessageKind::SurfaceVisible:
        case MessageKind::SurfaceHidden:
        case MessageKind::SurfaceReleased:
        case MessageKind::ShowSurface: return DecodeSurfaceInvocation(frame.Payload).has_value();
        case MessageKind::SurfaceFailed: return DecodeSurfaceFailure(frame.Payload).has_value();
        case MessageKind::AppliedRevision:
        case MessageKind::StateChanged: return DecodeRevisionNotice(frame.Payload).has_value();
        case MessageKind::ServerChallenge: return DecodeServerChallenge(frame.Payload).has_value();
        case MessageKind::ClientAuthenticate: return DecodeClientAuthenticate(frame.Payload).has_value();
        case MessageKind::ClientReconnect: return DecodeReconnectAuthenticate(frame.Payload).has_value();
        case MessageKind::ServerAccepted: return DecodeServerAccepted(frame.Payload).has_value();
        case MessageKind::SetDeviceConnectionState: return DecodeDeviceConnectionRequest(frame.Payload).has_value();
        case MessageKind::SetSetting: return DecodeSettingMutation(frame.Payload).has_value();
        case MessageKind::AppActionRequest: return DecodeAppAction(frame.Payload).has_value();
        case MessageKind::FullSnapshot: return DecodeSnapshot(frame.Payload).has_value();
        case MessageKind::CommandResult: return DecodeCommandResult(frame.Payload).has_value();
        case MessageKind::ProtocolError: return frame.Payload.size() <= c_maxPayloadBytes;
    }
    return false;
}

bool IsActiveFrameAllowed(Frame const& frame, std::uint64_t capabilities) noexcept {
    if (capabilities == 0 || (capabilities & ~c_knownCapabilities) != 0) return false;
    const auto hasCapability = [&](std::uint64_t required) noexcept { return (capabilities & required) != 0; };
    switch (frame.Kind) {
        case MessageKind::SetDeviceConnectionState:
        case MessageKind::RefreshInventory: return hasCapability(CapabilityDevicePicker) && IsFramePayloadValid(frame);
        case MessageKind::SetSetting: return hasCapability(CapabilitySettings) && IsFramePayloadValid(frame);
        case MessageKind::AppActionRequest: return hasCapability(CapabilityUpdates) && IsFramePayloadValid(frame);
        case MessageKind::Subscribe:
        case MessageKind::RequestFullSnapshot:
            return (capabilities & c_snapshotDomains) != 0 && IsFramePayloadValid(frame);
        case MessageKind::AppliedRevision:
        case MessageKind::StateChanged: {
            auto notice = DecodeRevisionNotice(frame.Payload);
            return notice && (notice->Domains & ~capabilities) == 0;
        }
        case MessageKind::SurfacePrepared:
        case MessageKind::SurfaceVisible:
        case MessageKind::SurfaceHidden:
        case MessageKind::SurfaceReleased:
        case MessageKind::ShowSurface: {
            auto invocation = DecodeSurfaceInvocation(frame.Payload);
            if (!invocation) return false;
            const auto required =
                invocation->Type == Surface::DevicePicker ? CapabilityDevicePicker : CapabilitySettings;
            if (!hasCapability(required)) return false;
            switch (frame.Kind) {
                case MessageKind::SurfacePrepared: return invocation->Intent == SurfaceIntent::Prepare;
                case MessageKind::SurfaceVisible:
                    return invocation->Intent == SurfaceIntent::Show || invocation->Intent == SurfaceIntent::Activate ||
                           invocation->Intent == SurfaceIntent::Toggle;
                case MessageKind::SurfaceHidden:
                    return invocation->Intent == SurfaceIntent::Hide || invocation->Intent == SurfaceIntent::Toggle;
                case MessageKind::SurfaceReleased: return invocation->Intent == SurfaceIntent::Release;
                case MessageKind::ShowSurface: return true;
                default: return false;
            }
        }
        case MessageKind::SurfaceFailed: {
            auto failure = DecodeSurfaceFailure(frame.Payload);
            if (!failure) return false;
            const auto required =
                failure->Invocation.Type == Surface::DevicePicker ? CapabilityDevicePicker : CapabilitySettings;
            return hasCapability(required);
        }
        case MessageKind::FullSnapshot: {
            auto snapshot = DecodeSnapshot(frame.Payload);
            return snapshot && (snapshot->Domains & ~capabilities) == 0;
        }
        default: return IsFramePayloadValid(frame);
    }
}

std::optional<SecureByteBuffer> EncodeFrameEnvelope(Frame const& frame) {
    if (!IsFrameEnvelopeValid(frame)) return std::nullopt;
    Encoder encoder(c_wireHeaderBytes + c_maxPayloadBytes, c_wireHeaderBytes + frame.Payload.size());
    if (!encoder.Append(c_frameMagic) || !encoder.Append(c_protocolMajor) || !encoder.Append(c_protocolMinor) ||
        !encoder.Append(c_wireHeaderBytes) || !encoder.Append(std::uint16_t{0}) ||
        !encoder.Append(static_cast<std::uint32_t>(frame.Kind)) || !encoder.Append(frame.Flags) ||
        !encoder.Append(static_cast<std::uint32_t>(frame.Payload.size())) || !encoder.Append(frame.Epoch.Bytes) ||
        !encoder.Append(frame.Connection.Bytes) || !encoder.Append(frame.Sequence) ||
        !encoder.Append(frame.Correlation.Bytes) || !encoder.Append(frame.Payload)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<Frame> DecodeFrameEnvelope(std::span<std::byte const> wireBytes) noexcept {
    try {
        if (wireBytes.size() < c_wireHeaderBytes) return std::nullopt;
        Decoder decoder(wireBytes);
        std::uint32_t magic = 0;
        std::uint16_t major = 0, minor = 0, headerBytes = 0, reserved = 0;
        std::uint32_t kind = 0, payloadBytes = 0;
        Frame frame;
        if (!decoder.Read(magic) || !decoder.Read(major) || !decoder.Read(minor) || !decoder.Read(headerBytes) ||
            !decoder.Read(reserved) || !decoder.Read(kind) || !decoder.Read(frame.Flags) ||
            !decoder.Read(payloadBytes) || !decoder.ReadBytes(frame.Epoch.Bytes) ||
            !decoder.ReadBytes(frame.Connection.Bytes) || !decoder.Read(frame.Sequence) ||
            !decoder.ReadBytes(frame.Correlation.Bytes) || magic != c_frameMagic || major != c_protocolMajor ||
            minor != c_protocolMinor || headerBytes != c_wireHeaderBytes || reserved != 0 ||
            payloadBytes > c_maxPayloadBytes || wireBytes.size() != c_wireHeaderBytes + payloadBytes ||
            !IsKnownMessageKind(kind)) {
            return std::nullopt;
        }
        frame.Kind = static_cast<MessageKind>(kind);
        frame.Payload.resize(payloadBytes);
        if (!decoder.ReadBytes(frame.Payload) || !decoder.Finished() || !IsFrameEnvelopeValid(frame)) {
            return std::nullopt;
        }
        return frame;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

bool IsFrameValid(Frame const& frame) noexcept {
    return IsFrameEnvelopeValid(frame) && IsFramePayloadValid(frame);
}

std::optional<SecureByteBuffer> EncodeFrame(Frame const& frame) {
    return IsFrameValid(frame) ? EncodeFrameEnvelope(frame) : std::nullopt;
}

std::optional<Frame> DecodeFrame(std::span<std::byte const> wireBytes) noexcept {
    auto frame = DecodeFrameEnvelope(wireBytes);
    return frame && IsFramePayloadValid(*frame) ? std::move(frame) : std::nullopt;
}

std::optional<SecureByteBuffer> EncodeServerChallenge(ServerChallenge const& value) {
    if (value.Launch.Empty() || value.ServerNonce.Empty() || value.ServerProcessId == 0 || value.ClientProcessId == 0 ||
        value.ServerCreationTime == 0 || value.ClientCreationTime == 0) {
        return std::nullopt;
    }
    Encoder encoder;
    if (!encoder.Append(value.Launch.Bytes) || !encoder.Append(value.ServerNonce.Bytes) ||
        !encoder.Append(value.ServerProcessId) || !encoder.Append(value.ClientProcessId) ||
        !encoder.Append(value.ServerCreationTime) || !encoder.Append(value.ClientCreationTime)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<ServerChallenge> DecodeServerChallenge(std::span<std::byte const> payload) noexcept {
    ServerChallenge value;
    Decoder decoder(payload);
    if (!decoder.ReadBytes(value.Launch.Bytes) || !decoder.ReadBytes(value.ServerNonce.Bytes) ||
        !decoder.Read(value.ServerProcessId) || !decoder.Read(value.ClientProcessId) ||
        !decoder.Read(value.ServerCreationTime) || !decoder.Read(value.ClientCreationTime) || !decoder.Finished() ||
        value.Launch.Empty() || value.ServerNonce.Empty() || value.ServerProcessId == 0 || value.ClientProcessId == 0 ||
        value.ServerCreationTime == 0 || value.ClientCreationTime == 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<SecureByteBuffer> EncodeClientAuthenticate(ClientAuthenticate const& value) {
    if (value.Launch.Empty() || value.ServerNonce.Empty() || value.ClientNonce.Empty() || value.Capabilities == 0 ||
        (value.Capabilities & ~c_knownCapabilities) != 0) {
        return std::nullopt;
    }
    Encoder encoder;
    if (!encoder.Append(value.Launch.Bytes) || !encoder.Append(value.ServerNonce.Bytes) ||
        !encoder.Append(value.ClientNonce.Bytes) || !encoder.Append(value.Capabilities)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<ClientAuthenticate> DecodeClientAuthenticate(std::span<std::byte const> payload) noexcept {
    ClientAuthenticate value;
    Decoder decoder(payload);
    if (!decoder.ReadBytes(value.Launch.Bytes) || !decoder.ReadBytes(value.ServerNonce.Bytes) ||
        !decoder.ReadBytes(value.ClientNonce.Bytes) || !decoder.Read(value.Capabilities) || !decoder.Finished() ||
        value.Launch.Empty() || value.ServerNonce.Empty() || value.ClientNonce.Empty() || value.Capabilities == 0 ||
        (value.Capabilities & ~c_knownCapabilities) != 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<SecureByteBuffer> EncodeReconnectAuthenticate(ReconnectAuthenticate const& value) {
    if (value.Launch.Empty() || value.ServerNonce.Empty() || value.ClientNonce.Empty() || value.Proof.Empty() ||
        value.Capabilities == 0 || (value.Capabilities & ~c_knownCapabilities) != 0) {
        return std::nullopt;
    }
    Encoder encoder;
    if (!encoder.Append(value.Launch.Bytes) || !encoder.Append(value.ServerNonce.Bytes) ||
        !encoder.Append(value.ClientNonce.Bytes) || !encoder.Append(value.Capabilities) ||
        !encoder.Append(value.Proof.Bytes)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<ReconnectAuthenticate> DecodeReconnectAuthenticate(std::span<std::byte const> payload) noexcept {
    ReconnectAuthenticate value;
    Decoder decoder(payload);
    if (!decoder.ReadBytes(value.Launch.Bytes) || !decoder.ReadBytes(value.ServerNonce.Bytes) ||
        !decoder.ReadBytes(value.ClientNonce.Bytes) || !decoder.Read(value.Capabilities) ||
        !decoder.ReadBytes(value.Proof.Bytes) || !decoder.Finished() || value.Launch.Empty() ||
        value.ServerNonce.Empty() || value.ClientNonce.Empty() || value.Proof.Empty() || value.Capabilities == 0 ||
        (value.Capabilities & ~c_knownCapabilities) != 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<SecureByteBuffer> EncodeServerAccepted(ServerAccepted const& value) {
    const bool reconnectNegotiated = (value.Capabilities & CapabilityReconnect) != 0;
    if (value.Capabilities == 0 || (value.Capabilities & ~c_knownCapabilities) != 0 ||
        reconnectNegotiated == value.ReconnectCapability.Empty()) {
        return std::nullopt;
    }
    Encoder encoder;
    if (!encoder.Append(value.ReconnectCapability.Bytes) || !encoder.Append(value.Capabilities)) return std::nullopt;
    return std::move(encoder).Finish();
}

std::optional<ServerAccepted> DecodeServerAccepted(std::span<std::byte const> payload) noexcept {
    ServerAccepted value;
    Decoder decoder(payload);
    if (!decoder.ReadBytes(value.ReconnectCapability.Bytes) || !decoder.Read(value.Capabilities) ||
        !decoder.Finished() || value.Capabilities == 0 || (value.Capabilities & ~c_knownCapabilities) != 0 ||
        ((value.Capabilities & CapabilityReconnect) != 0) == value.ReconnectCapability.Empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Secret256> ComputeReconnectProof(SessionCapability const& capability,
                                               HostEpoch const& epoch,
                                               LaunchId const& launch,
                                               ConnectionId const& connection,
                                               Secret256 const& serverNonce,
                                               Secret256 const& clientNonce,
                                               DWORD serverProcessId,
                                               DWORD clientProcessId,
                                               std::uint64_t capabilities) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    SecureByteBuffer hashObject;
    Secret256 result;
    auto cleanup = [&]() noexcept {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!hashObject.empty()) SecureZeroMemory(hashObject.data(), hashObject.size());
    };
    try {
        if (capability.Empty() || epoch.Empty() || launch.Empty() || connection.Empty() || serverNonce.Empty() ||
            clientNonce.Empty() || serverProcessId == 0 || clientProcessId == 0 || capabilities == 0 ||
            (capabilities & ~c_knownCapabilities) != 0 ||
            !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
            cleanup();
            return std::nullopt;
        }
        DWORD objectBytes = 0, copied = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm,
                                              BCRYPT_OBJECT_LENGTH,
                                              reinterpret_cast<PUCHAR>(&objectBytes),
                                              sizeof(objectBytes),
                                              &copied,
                                              0)) ||
            objectBytes == 0 || copied != sizeof(objectBytes)) {
            cleanup();
            return std::nullopt;
        }
        hashObject.resize(objectBytes);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm,
                                             &hash,
                                             reinterpret_cast<PUCHAR>(hashObject.data()),
                                             objectBytes,
                                             reinterpret_cast<PUCHAR>(const_cast<std::byte*>(capability.Bytes.data())),
                                             static_cast<ULONG>(capability.Bytes.size()),
                                             0))) {
            cleanup();
            return std::nullopt;
        }

        Encoder transcript(256);
        constexpr std::array domain{std::byte{'A'},
                                    std::byte{'P'},
                                    std::byte{'C'},
                                    std::byte{'U'},
                                    std::byte{'I'},
                                    std::byte{'R'},
                                    std::byte{'1'}};
        if (!transcript.Append(domain) || !transcript.Append(epoch.Bytes) || !transcript.Append(launch.Bytes) ||
            !transcript.Append(connection.Bytes) || !transcript.Append(serverNonce.Bytes) ||
            !transcript.Append(clientNonce.Bytes) || !transcript.Append(serverProcessId) ||
            !transcript.Append(clientProcessId) || !transcript.Append(capabilities)) {
            cleanup();
            return std::nullopt;
        }
        auto bytes = std::move(transcript).Finish();
        const bool hashed =
            BCRYPT_SUCCESS(
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(
                hash, reinterpret_cast<PUCHAR>(result.Bytes.data()), static_cast<ULONG>(result.Bytes.size()), 0));
        cleanup();
        return hashed && !result.Empty() ? std::optional(result) : std::nullopt;
    } catch (...) {
        cleanup();
        return std::nullopt;
    }
}

std::optional<SecureByteBuffer> EncodeRevisionNotice(RevisionNotice const& notice) {
    if (!IsRevisionNoticeValid(notice)) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(notice.Domains) || !AppendRevisions(encoder, notice.Revisions)) return std::nullopt;
    return std::move(encoder).Finish();
}

std::optional<RevisionNotice> DecodeRevisionNotice(std::span<std::byte const> payload) noexcept {
    RevisionNotice notice;
    Decoder decoder(payload);
    return decoder.Read(notice.Domains) && ReadRevisions(decoder, notice.Revisions) && decoder.Finished() &&
                   IsRevisionNoticeValid(notice)
               ? std::optional(notice)
               : std::nullopt;
}

std::optional<SecureByteBuffer> EncodeSurfaceInvocation(SurfaceInvocation const& invocation) {
    if (!IsSurfaceInvocationValid(invocation)) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(static_cast<std::uint32_t>(invocation.Type)) ||
        !encoder.Append(static_cast<std::uint32_t>(invocation.Intent)) ||
        !encoder.Append(invocation.Invocation.Bytes) || !encoder.Append(invocation.Flags) ||
        !encoder.Append(invocation.AnchorLeft) || !encoder.Append(invocation.AnchorTop) ||
        !encoder.Append(invocation.AnchorRight) || !encoder.Append(invocation.AnchorBottom) ||
        !encoder.Append(invocation.Dpi)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<SurfaceInvocation> DecodeSurfaceInvocation(std::span<std::byte const> payload) noexcept {
    Decoder decoder(payload);
    SurfaceInvocation invocation;
    std::uint32_t surface = 0, intent = 0;
    if (!decoder.Read(surface) || !decoder.Read(intent) || !decoder.ReadBytes(invocation.Invocation.Bytes) ||
        !decoder.Read(invocation.Flags) || !decoder.Read(invocation.AnchorLeft) ||
        !decoder.Read(invocation.AnchorTop) || !decoder.Read(invocation.AnchorRight) ||
        !decoder.Read(invocation.AnchorBottom) || !decoder.Read(invocation.Dpi) || !decoder.Finished() ||
        (surface != static_cast<std::uint32_t>(Surface::DevicePicker) &&
         surface != static_cast<std::uint32_t>(Surface::Settings))) {
        return std::nullopt;
    }
    invocation.Type = static_cast<Surface>(surface);
    invocation.Intent = static_cast<SurfaceIntent>(intent);
    return IsSurfaceInvocationValid(invocation) ? std::optional(invocation) : std::nullopt;
}

std::optional<SecureByteBuffer> EncodeSurfaceFailure(SurfaceFailure const& failure) {
    if (failure.Error == ERROR_SUCCESS) return std::nullopt;
    auto invocation = EncodeSurfaceInvocation(failure.Invocation);
    if (!invocation) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(*invocation) || !encoder.Append(failure.Error) ||
        !encoder.Append(std::uint32_t{failure.Retryable ? 1U : 0U})) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<SurfaceFailure> DecodeSurfaceFailure(std::span<std::byte const> payload) noexcept {
    try {
        constexpr std::size_t invocationBytes = 48;
        if (payload.size() != invocationBytes + sizeof(std::uint32_t) * 2) return std::nullopt;
        auto invocation = DecodeSurfaceInvocation(payload.first(invocationBytes));
        if (!invocation) return std::nullopt;
        Decoder decoder(payload.subspan(invocationBytes));
        SurfaceFailure failure;
        std::uint32_t retryable = 0;
        if (!decoder.Read(failure.Error) || !decoder.Read(retryable) || !decoder.Finished() ||
            failure.Error == ERROR_SUCCESS || retryable > 1) {
            return std::nullopt;
        }
        failure.Invocation = std::move(*invocation);
        failure.Retryable = retryable != 0;
        return failure;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SecureByteBuffer> EncodeAppAction(AppAction action) {
    if (!IsKnownAppAction(static_cast<std::uint32_t>(action))) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(static_cast<std::uint32_t>(action))) return std::nullopt;
    return std::move(encoder).Finish();
}

std::optional<AppAction> DecodeAppAction(std::span<std::byte const> payload) noexcept {
    Decoder decoder(payload);
    std::uint32_t action = 0;
    if (!decoder.Read(action) || !decoder.Finished() || !IsKnownAppAction(action)) return std::nullopt;
    return static_cast<AppAction>(action);
}

std::optional<SecureByteBuffer> EncodeSnapshot(Snapshot const& snapshot) {
    if (!IsSnapshotValid(snapshot)) return std::nullopt;
    Encoder encoder;
    if (!AppendRevisions(encoder, snapshot.Revisions) || !encoder.Append(snapshot.Domains) ||
        !encoder.Append(snapshot.Flags) || !encoder.Append(snapshot.Appearance) ||
        !encoder.Append(snapshot.ConnectedDeviceCount) ||
        !encoder.Append(static_cast<std::uint32_t>(snapshot.Inventory)) || !encoder.Append(snapshot.InventoryError) ||
        !encoder.Append(snapshot.KnownDeviceOffset) || !encoder.Append(snapshot.KnownDeviceTotal) ||
        !encoder.Append(snapshot.DeviceOffset) || !encoder.Append(snapshot.DeviceTotal) ||
        !encoder.Append(snapshot.Settings.Flags) ||
        !encoder.Append(static_cast<std::uint32_t>(snapshot.Settings.StartupState)) ||
        !encoder.AppendString(snapshot.Settings.Language) || !encoder.AppendString(snapshot.Settings.DefaultDeviceId) ||
        !encoder.Append(snapshot.Settings.Bounds.X) || !encoder.Append(snapshot.Settings.Bounds.Y) ||
        !encoder.Append(snapshot.Settings.Bounds.Width) || !encoder.Append(snapshot.Settings.Bounds.Height) ||
        !encoder.Append(snapshot.Settings.Bounds.Dpi) ||
        !encoder.Append(static_cast<std::uint32_t>(snapshot.Update.Status)) ||
        !encoder.AppendString(snapshot.Update.AvailableVersion) ||
        !encoder.Append(static_cast<std::uint32_t>(snapshot.KnownDevices.size()))) {
        return std::nullopt;
    }
    for (auto const& device : snapshot.KnownDevices) {
        if (!encoder.Append(device.Flags) || !encoder.AppendString(device.Id) || !encoder.AppendString(device.Name) ||
            !encoder.AppendString(device.Alias)) {
            return std::nullopt;
        }
    }
    if (!encoder.Append(static_cast<std::uint32_t>(snapshot.Devices.size()))) {
        return std::nullopt;
    }
    for (auto const& device : snapshot.Devices) {
        if (!encoder.Append(device.Flags) || !encoder.AppendString(device.Id) || !encoder.AppendString(device.Name) ||
            !encoder.AppendString(device.Alias) || !encoder.AppendString(device.DisplayName)) {
            return std::nullopt;
        }
    }
    return std::move(encoder).Finish();
}

std::optional<Snapshot> DecodeSnapshot(std::span<std::byte const> payload) noexcept {
    try {
        Decoder decoder(payload);
        Snapshot snapshot;
        std::uint32_t inventoryStatus = 0, startupState = 0, updateStatus = 0, knownDeviceCount = 0, deviceCount = 0;
        if (!ReadRevisions(decoder, snapshot.Revisions) || !decoder.Read(snapshot.Domains) ||
            !decoder.Read(snapshot.Flags) || !decoder.Read(snapshot.Appearance) ||
            !decoder.Read(snapshot.ConnectedDeviceCount) || !decoder.Read(inventoryStatus) ||
            !IsKnownInventoryStatus(inventoryStatus) || !decoder.Read(snapshot.InventoryError) ||
            !decoder.Read(snapshot.KnownDeviceOffset) || !decoder.Read(snapshot.KnownDeviceTotal) ||
            !decoder.Read(snapshot.DeviceOffset) || !decoder.Read(snapshot.DeviceTotal) ||
            !decoder.Read(snapshot.Settings.Flags) || !decoder.Read(startupState) ||
            !IsKnownStartupTaskState(startupState) || !decoder.ReadString(snapshot.Settings.Language) ||
            !decoder.ReadString(snapshot.Settings.DefaultDeviceId) || !decoder.Read(snapshot.Settings.Bounds.X) ||
            !decoder.Read(snapshot.Settings.Bounds.Y) || !decoder.Read(snapshot.Settings.Bounds.Width) ||
            !decoder.Read(snapshot.Settings.Bounds.Height) || !decoder.Read(snapshot.Settings.Bounds.Dpi) ||
            !decoder.Read(updateStatus) || !IsKnownUpdateStatus(updateStatus) ||
            !decoder.ReadString(snapshot.Update.AvailableVersion) || !decoder.Read(knownDeviceCount) ||
            knownDeviceCount > c_maxDeviceCount) {
            return std::nullopt;
        }
        snapshot.Inventory = static_cast<InventoryStatus>(inventoryStatus);
        snapshot.Settings.StartupState = static_cast<StartupTaskState>(startupState);
        snapshot.Update.Status = static_cast<UpdateStatus>(updateStatus);
        snapshot.KnownDevices.reserve(knownDeviceCount);
        for (std::uint32_t index = 0; index < knownDeviceCount; ++index) {
            KnownDeviceSettingsItem device;
            if (!decoder.Read(device.Flags) || !decoder.ReadString(device.Id) || !decoder.ReadString(device.Name) ||
                !decoder.ReadString(device.Alias)) {
                return std::nullopt;
            }
            snapshot.KnownDevices.push_back(std::move(device));
        }
        if (!decoder.Read(deviceCount) || deviceCount > c_maxDeviceCount) return std::nullopt;
        snapshot.Devices.reserve(deviceCount);
        for (std::uint32_t index = 0; index < deviceCount; ++index) {
            DeviceSnapshotItem device;
            if (!decoder.Read(device.Flags) || !decoder.ReadString(device.Id) || !decoder.ReadString(device.Name) ||
                !decoder.ReadString(device.Alias) || !decoder.ReadString(device.DisplayName)) {
                return std::nullopt;
            }
            snapshot.Devices.push_back(std::move(device));
        }
        return decoder.Finished() && IsSnapshotValid(snapshot) ? std::optional(std::move(snapshot)) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SecureByteBuffer> EncodeDeviceConnectionRequest(DeviceConnectionRequest const& request) {
    if (!IsDeviceRequestValid(request)) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(static_cast<std::uint32_t>(request.Type)) ||
        !encoder.Append(request.ExpectedActivityRevision) || !encoder.AppendString(request.DeviceId) ||
        !encoder.Append(std::uint32_t{request.ShouldBeConnected ? 1U : 0U})) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<DeviceConnectionRequest> DecodeDeviceConnectionRequest(std::span<std::byte const> payload) noexcept {
    try {
        Decoder decoder(payload);
        DeviceConnectionRequest request;
        std::uint32_t action = 0, connected = 0;
        if (!decoder.Read(action) || !IsKnownDeviceAction(action) || !decoder.Read(request.ExpectedActivityRevision) ||
            !decoder.ReadString(request.DeviceId) || !decoder.Read(connected) || !decoder.Finished() || connected > 1) {
            return std::nullopt;
        }
        request.Type = static_cast<DeviceAction>(action);
        request.ShouldBeConnected = connected != 0;
        return IsDeviceRequestValid(request) ? std::optional(std::move(request)) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SecureByteBuffer> EncodeSettingMutation(SettingMutation const& mutation) {
    if (!IsMutationValid(mutation)) return std::nullopt;
    Encoder encoder;
    if (!encoder.Append(static_cast<std::uint32_t>(mutation.Key)) ||
        !encoder.Append(static_cast<std::uint32_t>(mutation.ValueKind)) ||
        !encoder.Append(mutation.ExpectedSettingsRevision) || !encoder.AppendString(mutation.DeviceId)) {
        return std::nullopt;
    }
    switch (mutation.ValueKind) {
        case SettingValueKind::None: break;
        case SettingValueKind::Boolean:
            if (!encoder.Append(std::uint32_t{mutation.BooleanValue ? 1U : 0U})) return std::nullopt;
            break;
        case SettingValueKind::Integer:
            if (!encoder.Append(mutation.IntegerValue)) return std::nullopt;
            break;
        case SettingValueKind::String:
            if (!encoder.AppendString(mutation.StringValue)) return std::nullopt;
            break;
        case SettingValueKind::WindowBounds:
            if (!encoder.Append(mutation.BoundsValue.X) || !encoder.Append(mutation.BoundsValue.Y) ||
                !encoder.Append(mutation.BoundsValue.Width) || !encoder.Append(mutation.BoundsValue.Height) ||
                !encoder.Append(mutation.BoundsValue.Dpi)) {
                return std::nullopt;
            }
            break;
    }
    return std::move(encoder).Finish();
}

std::optional<SettingMutation> DecodeSettingMutation(std::span<std::byte const> payload) noexcept {
    try {
        Decoder decoder(payload);
        SettingMutation mutation;
        std::uint32_t key = 0, kind = 0;
        if (!decoder.Read(key) || !decoder.Read(kind) || !IsKnownSettingKey(key) || !IsKnownValueKind(kind) ||
            !decoder.Read(mutation.ExpectedSettingsRevision) || !decoder.ReadString(mutation.DeviceId)) {
            return std::nullopt;
        }
        mutation.Key = static_cast<SettingKey>(key);
        mutation.ValueKind = static_cast<SettingValueKind>(kind);
        switch (mutation.ValueKind) {
            case SettingValueKind::None: break;
            case SettingValueKind::Boolean: {
                std::uint32_t value = 0;
                if (!decoder.Read(value) || value > 1) return std::nullopt;
                mutation.BooleanValue = value != 0;
                break;
            }
            case SettingValueKind::Integer:
                if (!decoder.Read(mutation.IntegerValue)) return std::nullopt;
                break;
            case SettingValueKind::String:
                if (!decoder.ReadString(mutation.StringValue)) return std::nullopt;
                break;
            case SettingValueKind::WindowBounds:
                if (!decoder.Read(mutation.BoundsValue.X) || !decoder.Read(mutation.BoundsValue.Y) ||
                    !decoder.Read(mutation.BoundsValue.Width) || !decoder.Read(mutation.BoundsValue.Height) ||
                    !decoder.Read(mutation.BoundsValue.Dpi)) {
                    return std::nullopt;
                }
                break;
        }
        return decoder.Finished() && IsMutationValid(mutation) ? std::optional(std::move(mutation)) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SecureByteBuffer> EncodeCommandResult(CommandResult const& result) {
    if (!IsKnownActionStatus(static_cast<std::uint32_t>(result.Status)) || result.Revisions.State == 0 ||
        result.Message.size() > c_maxStringCharacters || result.Message.contains(L'\0')) {
        return std::nullopt;
    }
    Encoder encoder;
    if (!encoder.Append(static_cast<std::uint32_t>(result.Status)) || !AppendRevisions(encoder, result.Revisions) ||
        !encoder.AppendString(result.Message)) {
        return std::nullopt;
    }
    return std::move(encoder).Finish();
}

std::optional<CommandResult> DecodeCommandResult(std::span<std::byte const> payload) noexcept {
    try {
        Decoder decoder(payload);
        CommandResult result;
        std::uint32_t status = 0;
        if (!decoder.Read(status) || !IsKnownActionStatus(status) || !ReadRevisions(decoder, result.Revisions) ||
            result.Revisions.State == 0 || !decoder.ReadString(result.Message) || !decoder.Finished()) {
            return std::nullopt;
        }
        result.Status = static_cast<ActionStatus>(status);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {
std::uint64_t SystemTick() noexcept {
    return GetTickCount64();
}

[[nodiscard]] bool IsReplayCacheLimitsValid(ReplayCacheLimits const& limits) noexcept {
    return limits.LedgerEntries > 0 && limits.LedgerEntries <= c_maxReplayLedgerEntries && limits.ResultEntries > 0 &&
           limits.ResultEntries <= c_maxReplayResultEntries && limits.ResultBytes > 0 &&
           limits.ResultBytes <= c_maxReplayResultBytes && limits.PendingMilliseconds > 0 &&
           limits.PendingMilliseconds <= c_maxPendingRequestMilliseconds;
}
} // namespace

RequestReplayCache::RequestReplayCache(HostEpoch const& epoch, ReplayCacheLimits limits, TickSource tickSource) noexcept
    : m_epoch(IsReplayCacheLimitsValid(limits) ? epoch : HostEpoch{}), m_limits(limits),
      m_tickSource(tickSource ? tickSource : SystemTick) {}

RequestReplayCache::RequestReplayCache(HostEpoch const& epoch,
                                       bool retainCompleted,
                                       ReplayCacheLimits limits,
                                       TickSource tickSource) noexcept
    : m_epoch(IsReplayCacheLimitsValid(limits) ? epoch : HostEpoch{}), m_retainCompleted(retainCompleted),
      m_limits(limits), m_tickSource(tickSource ? tickSource : SystemTick) {}

std::size_t RequestReplayCache::CorrelationHash::operator()(CorrelationId const& value) const noexcept {
    std::size_t hash = sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1469598103934665603ULL)
                                                : static_cast<std::size_t>(2166136261U);
    constexpr std::size_t prime =
        sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1099511628211ULL) : static_cast<std::size_t>(16777619U);
    for (auto byte : value.Bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

bool RequestReplayCache::MakeRoomLocked(std::size_t bytes) noexcept {
    if (bytes > m_limits.ResultBytes) return false;
    while (m_resultCount >= m_limits.ResultEntries || m_resultBytes > m_limits.ResultBytes - bytes) {
        if (m_completedResults.empty()) return false;
        const auto correlation = m_completedResults.front();
        m_completedResults.pop_front();
        auto candidate = m_entries.find(correlation);
        if (candidate == m_entries.end() || !candidate->second.ResultPayload) continue;
        m_resultBytes -= candidate->second.ResultPayload->size();
        candidate->second.ResultPayload.reset();
        --m_resultCount;
    }
    return true;
}

ReplayObservation RequestReplayCache::Observe(Frame const& request) noexcept {
    return ObserveCore(request, false);
}

ReplayObservation RequestReplayCache::ObserveValidated(Frame const& request) noexcept {
    return ObserveCore(request, true);
}

ReplayObservation RequestReplayCache::ObserveCore(Frame const& request, bool syntaxValidated) noexcept {
    try {
        if (m_epoch.Empty() || !(request.Epoch == m_epoch) || !IsRequest(request.Kind) ||
            (!syntaxValidated && !IsFrameValid(request)) || request.Correlation.Empty()) {
            return {ReplayDisposition::Conflict, std::nullopt};
        }
        auto digest = ComputeSha256(request.Payload);
        if (!digest) return {ReplayDisposition::InternalFailure, std::nullopt};
        const bool replayProtected = IsReplayProtectedRequest(request.Kind);
        const auto now = m_tickSource();
        std::scoped_lock lock(m_mutex);
        auto existing = m_entries.find(request.Correlation);
        if (existing != m_entries.end()) {
            auto& entry = existing->second;
            if (entry.Kind != request.Kind || !ConstantTimeEqual(entry.RequestDigest, *digest)) {
                return {ReplayDisposition::Conflict, std::nullopt};
            }
            if (entry.ResultPayload) {
                return {ReplayDisposition::DuplicateCompleted, entry.ResultPayload};
            }
            if (entry.Completed) {
                return {ReplayDisposition::DuplicateResultUnavailable, std::nullopt};
            }
            if (entry.Expired || now - entry.CreatedAt >= m_limits.PendingMilliseconds) {
                entry.Expired = true;
                return {ReplayDisposition::PendingExpired, std::nullopt};
            }
            return {ReplayDisposition::DuplicatePending, std::nullopt};
        }
        if (m_entries.size() >= m_limits.LedgerEntries) {
            return {ReplayDisposition::CapacityExceeded, std::nullopt};
        }
        m_entries.emplace(request.Correlation,
                          Entry{request.Kind, replayProtected, false, false, *digest, std::nullopt, now});
        return {ReplayDisposition::NewRequest, std::nullopt};
    } catch (...) {
        return {ReplayDisposition::InternalFailure, std::nullopt};
    }
}

ReplayCompletion RequestReplayCache::Complete(CorrelationId const& correlation,
                                              SecureByteBuffer const& resultPayload) noexcept {
    return CompleteCore(correlation, resultPayload, false);
}

ReplayCompletion RequestReplayCache::CompleteValidated(CorrelationId const& correlation,
                                                       SecureByteBuffer const& resultPayload) noexcept {
    return CompleteCore(correlation, resultPayload, true);
}

ReplayCompletion RequestReplayCache::CompleteCore(CorrelationId const& correlation,
                                                  SecureByteBuffer const& resultPayload,
                                                  bool syntaxValidated) noexcept {
    try {
        if (!syntaxValidated && !DecodeCommandResult(resultPayload)) return ReplayCompletion::Conflict;
        std::scoped_lock lock(m_mutex);
        auto existing = m_entries.find(correlation);
        if (existing == m_entries.end()) return ReplayCompletion::NotTracked;
        auto& entry = existing->second;
        if (!entry.ReplayProtected || !m_retainCompleted) {
            m_entries.erase(existing);
            return ReplayCompletion::Stored;
        }
        if (entry.Completed) {
            return entry.ResultPayload && *entry.ResultPayload == resultPayload ? ReplayCompletion::AlreadyStored
                                                                                : ReplayCompletion::Conflict;
        }
        SecureByteBuffer resultCopy = resultPayload;
        if (!MakeRoomLocked(resultPayload.size())) return ReplayCompletion::CapacityExceeded;
        m_completedResults.push_back(correlation);
        entry.Completed = true;
        entry.Expired = false;
        entry.ResultPayload = std::move(resultCopy);
        m_resultBytes += resultPayload.size();
        ++m_resultCount;
        return ReplayCompletion::Stored;
    } catch (...) {
        return ReplayCompletion::InternalFailure;
    }
}

void RequestReplayCache::Abandon(CorrelationId const& correlation) noexcept {
    try {
        std::scoped_lock lock(m_mutex);
        auto existing = m_entries.find(correlation);
        if (existing != m_entries.end() && !existing->second.Completed) {
            m_entries.erase(existing);
        }
    } catch (...) {
    }
}

bool RequestReplayCache::MatchesEpoch(HostEpoch const& epoch) const noexcept {
    return !m_epoch.Empty() && m_epoch == epoch;
}

static IoStatus ReadFrameRaw(HANDLE pipe,
                             Frame& frame,
                             HANDLE stopEvent,
                             std::uint64_t deadline,
                             HANDLE completionEvent,
                             HANDLE shutdownEvent) noexcept {
    try {
        std::array<std::byte, c_wireHeaderBytes> header{};
        auto status = apc::control::ReadExact(pipe,
                                              header.data(),
                                              static_cast<std::uint32_t>(header.size()),
                                              stopEvent,
                                              deadline,
                                              completionEvent,
                                              shutdownEvent);
        if (status != apc::control::IoStatus::Success) return MapIoStatus(status);
        Frame received;
        auto payloadBytes = DecodeFrameHeader(header, received);
        if (!payloadBytes) return IoStatus::InvalidData;
        received.Payload.resize(*payloadBytes);
        if (*payloadBytes > 0) {
            status = apc::control::ReadExact(
                pipe, received.Payload.data(), *payloadBytes, stopEvent, deadline, completionEvent, shutdownEvent);
            if (status != apc::control::IoStatus::Success) return MapIoStatus(status);
        }
        frame = std::move(received);
        return IoStatus::Success;
    } catch (...) {
        return IoStatus::Failed;
    }
}

static IoStatus WriteWireRaw(HANDLE pipe,
                             std::span<std::byte const> wire,
                             HANDLE stopEvent,
                             std::uint64_t deadline,
                             HANDLE completionEvent,
                             HANDLE shutdownEvent) noexcept {
    try {
        return MapIoStatus(apc::control::WriteExact(pipe,
                                                    wire.data(),
                                                    static_cast<std::uint32_t>(wire.size()),
                                                    stopEvent,
                                                    deadline,
                                                    completionEvent,
                                                    shutdownEvent));
    } catch (...) {
        return IoStatus::Failed;
    }
}

SessionEndpoint::UniquePipeHandle::~UniquePipeHandle() noexcept {
    if (*this) CloseHandle(m_value);
}

SessionEndpoint::UniquePipeHandle::UniquePipeHandle(UniquePipeHandle&& other) noexcept : m_value(other.Release()) {}

SessionEndpoint::UniquePipeHandle& SessionEndpoint::UniquePipeHandle::operator=(UniquePipeHandle&& other) noexcept {
    if (this != &other) {
        if (*this) CloseHandle(m_value);
        m_value = other.Release();
    }
    return *this;
}

HANDLE SessionEndpoint::UniquePipeHandle::Release() noexcept {
    return std::exchange(m_value, INVALID_HANDLE_VALUE);
}

std::unique_ptr<SessionEndpoint>
SessionEndpoint::CreateBound(SessionRole role,
                             UniquePipeHandle connectedPipe,
                             HANDLE expectedPeerProcess,
                             ProcessBinding const& expectedPeer,
                             HostEpoch const& epoch,
                             ConnectionId const& connection,
                             SessionHandshakeContext handshake,
                             std::shared_ptr<RequestReplayCache> replayCache) noexcept {
    try {
        if (!connectedPipe || !expectedPeerProcess || epoch.Empty() || connection.Empty() ||
            (role == SessionRole::Server && (!replayCache || !replayCache->MatchesEpoch(epoch))) ||
            (role == SessionRole::Client && replayCache)) {
            return nullptr;
        }
        if (role == SessionRole::Client) {
            replayCache = std::shared_ptr<RequestReplayCache>(new RequestReplayCache(epoch, false, {}, nullptr));
        }
        std::uint64_t localCreationTime = 0;
        const auto& challenge = handshake.Challenge;
        if (!QueryCreationTime(GetCurrentProcess(), localCreationTime)) return nullptr;
        const bool contextBound = role == SessionRole::Server
                                      ? challenge.ServerProcessId == GetCurrentProcessId() &&
                                            challenge.ServerCreationTime == localCreationTime &&
                                            challenge.ClientProcessId == expectedPeer.ProcessId &&
                                            challenge.ClientCreationTime == expectedPeer.CreationTime
                                      : challenge.ClientProcessId == GetCurrentProcessId() &&
                                            challenge.ClientCreationTime == localCreationTime &&
                                            challenge.ServerProcessId == expectedPeer.ProcessId &&
                                            challenge.ServerCreationTime == expectedPeer.CreationTime;
        if (!contextBound) return nullptr;
        const bool trusted = role == SessionRole::Server
                                 ? IsExpectedTrustedPipeClient(connectedPipe.Get(), expectedPeerProcess, expectedPeer)
                                 : IsExpectedTrustedPipeServer(connectedPipe.Get(), expectedPeerProcess, expectedPeer);
        if (!trusted) return nullptr;
        std::unique_ptr<SessionEndpoint> endpoint(new (std::nothrow) SessionEndpoint(
            role, connectedPipe.Get(), epoch, connection, std::move(handshake), std::move(replayCache)));
        if (!endpoint) return nullptr;
        static_cast<void>(connectedPipe.Release());
        if (endpoint->Phase() == SessionPhase::Closed) return nullptr;
        return endpoint;
    } catch (...) {
        return nullptr;
    }
}

SessionEndpoint::SessionEndpoint(SessionRole role,
                                 HANDLE ownedPipe,
                                 HostEpoch const& epoch,
                                 ConnectionId const& connection,
                                 SessionHandshakeContext handshake,
                                 std::shared_ptr<RequestReplayCache> replayCache) noexcept
    : m_role(role), m_pipe(ownedPipe), m_epoch(epoch), m_connection(connection),
      m_state(role, epoch, connection, std::move(handshake)), m_replayCache(std::move(replayCache)),
      m_shutdownEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      m_readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      m_writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!m_shutdownEvent || !m_readEvent || !m_writeEvent) m_state.Close();
}

SessionEndpoint::~SessionEndpoint() noexcept {
    Close();
}

void SessionEndpoint::SignalTerminalFailureFromIo() noexcept {
    m_closing.store(true, std::memory_order_release);
    m_state.Close();
    if (m_shutdownEvent) SetEvent(m_shutdownEvent);
    if (m_pipe != INVALID_HANDLE_VALUE) CancelIoEx(m_pipe, nullptr);
}

IoStatus SessionEndpoint::Send(OutboundMessage message, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    return SendCore(std::move(message), stopEvent, deadline, false);
}

IoStatus SessionEndpoint::SendCore(OutboundMessage message,
                                   HANDLE stopEvent,
                                   std::uint64_t deadline,
                                   bool validatedReplay) noexcept {
    bool abandonRequest = false;
    CorrelationId reservedCorrelation;
    try {
        std::scoped_lock lock(m_writeMutex);
        if (m_closing.load(std::memory_order_acquire) || m_pipe == INVALID_HANDLE_VALUE ||
            m_state.Phase() == SessionPhase::Closed ||
            m_nextOutboundSequence == std::numeric_limits<std::uint64_t>::max() ||
            (validatedReplay && (m_role != SessionRole::Server || message.Kind != MessageKind::CommandResult))) {
            SignalTerminalFailureFromIo();
            return IoStatus::Closed;
        }
        Frame frame{message.Kind,
                    0,
                    m_epoch,
                    m_connection,
                    m_nextOutboundSequence,
                    message.Correlation,
                    std::move(message.Payload)};
        if (m_role == SessionRole::Client && IsRequest(frame.Kind)) {
            if (!m_replayCache) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            const auto observation = m_replayCache->ObserveValidated(frame);
            if (observation.Disposition == ReplayDisposition::Conflict) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            if (observation.Disposition == ReplayDisposition::DuplicatePending) {
                return IoStatus::DuplicateRequestPending;
            }
            if (observation.Disposition == ReplayDisposition::DuplicateResultUnavailable) {
                return IoStatus::DuplicateRequestResultUnavailable;
            }
            if (observation.Disposition == ReplayDisposition::PendingExpired) {
                return IoStatus::DuplicateRequestExpired;
            }
            if (observation.Disposition == ReplayDisposition::DuplicateCompleted) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            if (observation.Disposition == ReplayDisposition::CapacityExceeded) {
                SignalTerminalFailureFromIo();
                return IoStatus::EpochRotationRequired;
            }
            if (observation.Disposition == ReplayDisposition::InternalFailure) {
                SignalTerminalFailureFromIo();
                return IoStatus::Failed;
            }
            abandonRequest = observation.Disposition == ReplayDisposition::NewRequest;
            if (abandonRequest) reservedCorrelation = frame.Correlation;
        }
        auto wire = EncodeFrameEnvelope(frame);
        if (!wire || !m_state.ValidateAndAdvance(frame, FrameDirection::Outbound)) {
            if (abandonRequest) m_replayCache->Abandon(frame.Correlation);
            SignalTerminalFailureFromIo();
            return IoStatus::InvalidData;
        }
        if (m_role == SessionRole::Server && frame.Kind == MessageKind::CommandResult && !validatedReplay) {
            if (!m_replayCache) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            const auto completion = m_replayCache->CompleteValidated(frame.Correlation, frame.Payload);
            if (completion == ReplayCompletion::CapacityExceeded || completion == ReplayCompletion::InternalFailure) {
                SignalTerminalFailureFromIo();
                return IoStatus::Failed;
            }
            if (completion != ReplayCompletion::Stored) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
        }
        const auto status = WriteWireRaw(m_pipe, *wire, stopEvent, deadline, m_writeEvent, m_shutdownEvent);
        if (status != IoStatus::Success && abandonRequest) m_replayCache->Abandon(frame.Correlation);
        if (status != IoStatus::Success) SignalTerminalFailureFromIo();
        if (status == IoStatus::Success) {
            ++m_nextOutboundSequence;
            if (m_state.Phase() == SessionPhase::Closed) SignalTerminalFailureFromIo();
        }
        return status;
    } catch (...) {
        if (abandonRequest && m_replayCache) m_replayCache->Abandon(reservedCorrelation);
        Close();
        return IoStatus::Failed;
    }
}

IoStatus SessionEndpoint::Receive(Frame& frame, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    try {
        std::unique_lock lock(m_readMutex);
        if (m_closing.load(std::memory_order_acquire) || m_pipe == INVALID_HANDLE_VALUE ||
            m_state.Phase() == SessionPhase::Closed) {
            return IoStatus::Closed;
        }
        Frame received;
        const auto status = ReadFrameRaw(m_pipe, received, stopEvent, deadline, m_readEvent, m_shutdownEvent);
        if (status != IoStatus::Success) {
            SignalTerminalFailureFromIo();
            return status;
        }
        if (!m_state.ValidateAndAdvance(received, FrameDirection::Inbound)) {
            SignalTerminalFailureFromIo();
            return IoStatus::InvalidData;
        }
        if (m_role == SessionRole::Client && received.Kind == MessageKind::CommandResult) {
            if (!m_replayCache) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            const auto completion = m_replayCache->CompleteValidated(received.Correlation, received.Payload);
            if (completion == ReplayCompletion::CapacityExceeded || completion == ReplayCompletion::InternalFailure) {
                SignalTerminalFailureFromIo();
                return IoStatus::Failed;
            }
            if (completion != ReplayCompletion::Stored) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
        }
        if (m_role == SessionRole::Server && IsRequest(received.Kind)) {
            if (!m_replayCache) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            auto replay = m_replayCache->ObserveValidated(received);
            if (replay.Disposition == ReplayDisposition::Conflict) {
                SignalTerminalFailureFromIo();
                return IoStatus::InvalidData;
            }
            if (replay.Disposition == ReplayDisposition::CapacityExceeded) {
                SignalTerminalFailureFromIo();
                return IoStatus::EpochRotationRequired;
            }
            if (replay.Disposition == ReplayDisposition::InternalFailure) {
                SignalTerminalFailureFromIo();
                return IoStatus::Failed;
            }
            if (replay.Disposition == ReplayDisposition::DuplicatePending) {
                frame = std::move(received);
                return IoStatus::DuplicateRequestPending;
            }
            if (replay.Disposition == ReplayDisposition::DuplicateResultUnavailable) {
                frame = std::move(received);
                return IoStatus::DuplicateRequestResultUnavailable;
            }
            if (replay.Disposition == ReplayDisposition::PendingExpired) {
                frame = std::move(received);
                return IoStatus::DuplicateRequestExpired;
            }
            if (replay.Disposition == ReplayDisposition::DuplicateCompleted) {
                if (!replay.CachedResult) {
                    SignalTerminalFailureFromIo();
                    return IoStatus::InvalidData;
                }
                OutboundMessage response{
                    MessageKind::CommandResult, received.Correlation, std::move(*replay.CachedResult)};
                lock.unlock();
                const auto replayStatus = SendCore(std::move(response), stopEvent, deadline, true);
                if (replayStatus != IoStatus::Success) return replayStatus;
                frame = std::move(received);
                return IoStatus::DuplicateRequestReplayed;
            }
        }
        frame = std::move(received);
        if (m_state.Phase() == SessionPhase::Closed) SignalTerminalFailureFromIo();
        return IoStatus::Success;
    } catch (...) {
        Close();
        return IoStatus::Failed;
    }
}

void SessionEndpoint::Close() noexcept {
    try {
        std::scoped_lock closeLock(m_closeMutex);
        m_closing.store(true, std::memory_order_release);
        if (m_shutdownEvent) SetEvent(m_shutdownEvent);
        if (m_pipe != INVALID_HANDLE_VALUE) CancelIoEx(m_pipe, nullptr);
        std::scoped_lock ioLock(m_readMutex, m_writeMutex);
        m_state.Close();
        if (const HANDLE pipe = std::exchange(m_pipe, INVALID_HANDLE_VALUE); pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        if (const HANDLE event = std::exchange(m_readEvent, nullptr)) CloseHandle(event);
        if (const HANDLE event = std::exchange(m_writeEvent, nullptr)) CloseHandle(event);
        if (const HANDLE event = std::exchange(m_shutdownEvent, nullptr)) CloseHandle(event);
    } catch (...) {
        m_state.Close();
    }
}

SessionPhase SessionEndpoint::Phase() const noexcept {
    return m_state.Phase();
}

std::uint64_t SessionEndpoint::NegotiatedCapabilities() const noexcept {
    return m_state.NegotiatedCapabilities();
}

std::optional<SessionCapability> SessionEndpoint::EstablishedCapability() const noexcept {
    return m_state.EstablishedCapability();
}

} // namespace apc::core_ui
