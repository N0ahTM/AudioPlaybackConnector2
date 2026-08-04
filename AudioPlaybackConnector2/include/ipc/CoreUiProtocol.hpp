#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <control/CommandPipeSecurity.hpp>
#include <core/SettingsLimits.hpp>

namespace apc::core_ui {

template <typename T> class SecureAllocator {
public:
    using value_type = T;

    SecureAllocator() noexcept = default;
    template <typename U> SecureAllocator(SecureAllocator<U> const&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
    void deallocate(T* pointer, std::size_t count) noexcept {
        if (!pointer) return;
        SecureZeroMemory(pointer, count * sizeof(T));
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template <typename U> [[nodiscard]] bool operator==(SecureAllocator<U> const&) const noexcept { return true; }
};

using SecureByteBuffer = std::vector<std::byte, SecureAllocator<std::byte>>;

inline constexpr std::uint16_t c_protocolMajor = 1;
inline constexpr std::uint16_t c_protocolMinor = 0;
inline constexpr std::uint32_t c_frameMagic = 0x31495541;
inline constexpr std::uint16_t c_wireHeaderBytes = 80;
inline constexpr std::uint32_t c_maxPayloadBytes = 256 * 1024;
inline constexpr std::uint32_t c_maxDeviceCount = 32;
inline constexpr std::uint32_t c_maxTotalDeviceCount =
    static_cast<std::uint32_t>(apc::limits::c_maxPersistedDeviceCount);
inline constexpr std::uint32_t c_maxDeviceIdCharacters =
    static_cast<std::uint32_t>(apc::limits::c_maxDeviceIdCharacters);
inline constexpr std::uint32_t c_maxDeviceNameCharacters =
    static_cast<std::uint32_t>(apc::limits::c_maxDeviceNameCharacters);
inline constexpr std::uint32_t c_maxDeviceAliasCharacters =
    static_cast<std::uint32_t>(apc::limits::c_maxDeviceAliasCharacters);
inline constexpr std::uint32_t c_maxTextCharacters = c_maxDeviceNameCharacters;
inline constexpr std::uint32_t c_maxLanguageCharacters =
    static_cast<std::uint32_t>(apc::limits::c_maxLanguageCharacters);
inline constexpr std::uint32_t c_maxVersionCharacters = static_cast<std::uint32_t>(apc::limits::c_maxVersionCharacters);
inline constexpr std::uint32_t c_maxStringCharacters = 16 * 1024;
inline constexpr std::size_t c_maxReplayLedgerEntries = 16 * 1024;
inline constexpr std::size_t c_maxReplayResultEntries = 256;
inline constexpr std::size_t c_maxReplayResultBytes = 2 * 1024 * 1024;
inline constexpr std::uint64_t c_maxPendingRequestMilliseconds = 60 * 60 * 1000;
inline constexpr std::wstring_view c_pipeNamePrefix = L"\\\\.\\pipe\\AudioPlaybackConnector2.CoreUi.v1.";

struct Identifier128 {
    std::array<std::byte, 16> Bytes{};

    [[nodiscard]] bool Empty() const noexcept;
    friend bool operator==(Identifier128 const& lhs, Identifier128 const& rhs) noexcept;
};

struct Secret256 {
    std::array<std::byte, 32> Bytes{};

    Secret256() noexcept = default;
    Secret256(Secret256 const&) = default;
    Secret256& operator=(Secret256 const&) = default;
    Secret256(Secret256&& other) noexcept;
    Secret256& operator=(Secret256&& other) noexcept;
    ~Secret256() noexcept;

    [[nodiscard]] bool Empty() const noexcept;
    void Clear() noexcept;
    friend bool operator==(Secret256 const& lhs, Secret256 const& rhs) noexcept;
};

using HostEpoch = Identifier128;
using LaunchId = Identifier128;
using ConnectionId = Identifier128;
using CorrelationId = Identifier128;
using SessionCapability = Secret256;

enum class MessageKind : std::uint32_t {
    ServerChallenge = 1,
    ClientAuthenticate = 2,
    ServerAccepted = 3,
    ClientReconnect = 4,
    Subscribe = 10,
    RequestFullSnapshot = 11,
    SetDeviceConnectionState = 12,
    SetSetting = 13,
    RefreshInventory = 14,
    SurfacePrepared = 15,
    SurfaceVisible = 16,
    AppliedRevision = 17,
    UiClosing = 18,
    AppActionRequest = 19,
    SurfaceHidden = 20,
    SurfaceReleased = 21,
    SurfaceFailed = 22,
    FullSnapshot = 30,
    StateChanged = 31,
    CommandResult = 32,
    ShowSurface = 33,
    CloseRequested = 34,
    ProtocolError = 35,
};

enum class Surface : std::uint32_t { DevicePicker = 1, Settings = 2 };
enum class SurfaceIntent : std::uint32_t { Prepare = 1, Show = 2, Activate = 3, Hide = 4, Toggle = 5, Release = 6 };
enum class DeviceAction : std::uint32_t { SetConnected = 1, Reconnect = 2, DisconnectAll = 3, ReconnectAll = 4 };
enum class AppAction : std::uint32_t { CheckForUpdates = 1, OpenAvailableUpdate = 2 };
enum class ActionStatus : std::uint32_t {
    Success = 0,
    InvalidRequest = 1,
    Conflict = 2,
    NotFound = 3,
    Busy = 4,
    Failed = 5
};
enum class StartupTaskState : std::uint32_t {
    Unknown = 0,
    Disabled = 1,
    Enabled = 2,
    DisabledByUser = 3,
    Unavailable = 4,
    EnabledByPolicy = 5,
    DisabledByPolicy = 6
};
enum class UpdateStatus : std::uint32_t { Idle = 0, Checking = 1, Current = 2, Available = 3, Failed = 4 };
enum class InventoryStatus : std::uint32_t { NotStarted = 0, Refreshing = 1, Ready = 2, Failed = 3, Cancelled = 4 };
enum class SettingValueKind : std::uint32_t { None = 0, Boolean = 1, Integer = 2, String = 3, WindowBounds = 4 };
enum class SettingKey : std::uint32_t {
    GlobalConnectOnStartup = 1,
    GlobalReconnectOnConnectionLoss = 2,
    AllowIncomingConnections = 3,
    StartWithWindows = 4,
    ShowNotifications = 5,
    Language = 6,
    PrivacyMode = 7,
    SettingsWindowBounds = 8,
    DeviceConnectOnStartup = 9,
    DeviceReconnectOnConnectionLoss = 10,
    DeviceAlias = 11,
    DefaultDevice = 12,
    ForgetDevice = 14,
};

enum SnapshotFlags : std::uint32_t {
    SnapshotFlagNone = 0,
    SnapshotFlagInventoryFresh = 1U << 0,
    SnapshotFlagHasCachedInventory = 1U << 1,
};

enum DeviceFlags : std::uint32_t {
    DeviceFlagNone = 0,
    DeviceFlagConnected = 1U << 0,
    DeviceFlagBusy = 1U << 1,
};

enum KnownDeviceFlags : std::uint32_t {
    KnownDeviceFlagNone = 0,
    KnownDeviceFlagConnectOnStartup = 1U << 0,
    KnownDeviceFlagReconnectOnConnectionLoss = 1U << 1,
};

enum SettingsFlags : std::uint32_t {
    SettingsFlagNone = 0,
    SettingsFlagGlobalConnectOnStartup = 1U << 0,
    SettingsFlagGlobalReconnectOnConnectionLoss = 1U << 1,
    SettingsFlagAllowIncomingConnections = 1U << 2,
    SettingsFlagStartWithWindowsRequested = 1U << 3,
    SettingsFlagShowNotifications = 1U << 4,
    SettingsFlagPrivacyMode = 1U << 5,
    SettingsFlagSpecificDefaultDevice = 1U << 6,
    SettingsFlagHasWindowBounds = 1U << 7,
};

enum AppearanceFlags : std::uint32_t {
    AppearanceFlagNone = 0,
    AppearanceFlagAllowMica = 1U << 0,
    AppearanceFlagAllowAcrylic = 1U << 1,
    AppearanceFlagTransparencyEnabled = 1U << 2,
    AppearanceFlagHighContrast = 1U << 3,
    AppearanceFlagRemoteSession = 1U << 4,
    AppearanceFlagResourceConstrained = 1U << 5,
};

enum SurfaceFlags : std::uint32_t {
    SurfaceFlagNone = 0,
    SurfaceFlagHasAnchor = 1U << 0,
    SurfaceFlagUserInitiated = 1U << 1,
};

enum CapabilityFlags : std::uint64_t {
    CapabilityNone = 0,
    CapabilityDevicePicker = 1ULL << 0,
    CapabilitySettings = 1ULL << 1,
    CapabilityAppearancePolicy = 1ULL << 2,
    CapabilityReconnect = 1ULL << 3,
    CapabilityUpdates = 1ULL << 4,
};

inline constexpr std::uint64_t c_knownCapabilities =
    CapabilityDevicePicker | CapabilitySettings | CapabilityAppearancePolicy | CapabilityReconnect | CapabilityUpdates;

struct Frame {
    MessageKind Kind = MessageKind::ProtocolError;
    std::uint32_t Flags = 0;
    HostEpoch Epoch;
    ConnectionId Connection;
    std::uint64_t Sequence = 0;
    CorrelationId Correlation;
    SecureByteBuffer Payload;
};

struct OutboundMessage {
    MessageKind Kind = MessageKind::ProtocolError;
    CorrelationId Correlation;
    SecureByteBuffer Payload;
};

struct ProcessBinding {
    DWORD ProcessId = 0;
    std::uint64_t CreationTime = 0;
    std::wstring ImagePath;
    std::optional<apc::control::ExecutableFileIdentity> ExecutableIdentity;
};

struct ServerChallenge {
    LaunchId Launch;
    Secret256 ServerNonce;
    DWORD ServerProcessId = 0;
    DWORD ClientProcessId = 0;
    std::uint64_t ServerCreationTime = 0;
    std::uint64_t ClientCreationTime = 0;
};

struct ClientAuthenticate {
    LaunchId Launch;
    Secret256 ServerNonce;
    Secret256 ClientNonce;
    std::uint64_t Capabilities = 0;
};

struct ReconnectAuthenticate {
    LaunchId Launch;
    Secret256 ServerNonce;
    Secret256 ClientNonce;
    std::uint64_t Capabilities = 0;
    Secret256 Proof;
};

struct ServerAccepted {
    SessionCapability ReconnectCapability;
    std::uint64_t Capabilities = 0;
};

struct SessionHandshakeContext {
    ServerChallenge Challenge;
    std::uint64_t SupportedCapabilities = 0;
    std::optional<SessionCapability> ReconnectCapability;
};

struct UiLaunchBootstrap {
    LaunchId Launch;
    HostEpoch Epoch;
    ConnectionId Connection;
    std::uintptr_t InheritedCoreProcessHandle = 0;
    DWORD CoreProcessId = 0;
    std::uint64_t CoreCreationTime = 0;
    std::uint64_t SupportedCapabilities = 0;
};

struct StateRevisions {
    std::uint64_t State = 0;
    std::uint64_t Inventory = 0;
    std::uint64_t Activity = 0;
    std::uint64_t Settings = 0;
    std::uint64_t Presentation = 0;

    bool operator==(StateRevisions const&) const = default;
};

struct RevisionNotice {
    std::uint64_t Domains = CapabilityNone;
    StateRevisions Revisions;

    bool operator==(RevisionNotice const&) const = default;
};

struct WindowBounds {
    std::int32_t X = 0;
    std::int32_t Y = 0;
    std::int32_t Width = 0;
    std::int32_t Height = 0;
    std::uint32_t Dpi = USER_DEFAULT_SCREEN_DPI;

    bool operator==(WindowBounds const&) const = default;
};

struct SettingsSnapshot {
    std::uint32_t Flags = SettingsFlagNone;
    StartupTaskState StartupState = StartupTaskState::Unknown;
    std::wstring Language = L"system";
    std::wstring DefaultDeviceId;
    WindowBounds Bounds;

    bool operator==(SettingsSnapshot const&) const = default;
};

struct UpdateSnapshot {
    UpdateStatus Status = UpdateStatus::Idle;
    std::wstring AvailableVersion;

    bool operator==(UpdateSnapshot const&) const = default;
};

struct DeviceSnapshotItem {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    std::uint32_t Flags = DeviceFlagNone;

    bool operator==(DeviceSnapshotItem const&) const = default;
};

struct KnownDeviceSettingsItem {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::uint32_t Flags = KnownDeviceFlagNone;

    bool operator==(KnownDeviceSettingsItem const&) const = default;
};

struct Snapshot {
    StateRevisions Revisions;
    std::uint64_t Domains = CapabilityNone;
    std::uint32_t Flags = SnapshotFlagNone;
    std::uint32_t Appearance = AppearanceFlagNone;
    std::uint32_t ConnectedDeviceCount = 0;
    InventoryStatus Inventory = InventoryStatus::NotStarted;
    std::uint32_t InventoryError = ERROR_SUCCESS;
    std::uint32_t KnownDeviceOffset = 0;
    std::uint32_t KnownDeviceTotal = 0;
    std::uint32_t DeviceOffset = 0;
    std::uint32_t DeviceTotal = 0;
    SettingsSnapshot Settings;
    UpdateSnapshot Update;
    std::vector<KnownDeviceSettingsItem> KnownDevices;
    std::vector<DeviceSnapshotItem> Devices;

    bool operator==(Snapshot const&) const = default;
};

struct DeviceConnectionRequest {
    DeviceAction Type = DeviceAction::SetConnected;
    std::uint64_t ExpectedActivityRevision = 0;
    std::wstring DeviceId;
    bool ShouldBeConnected = false;
};

struct SurfaceInvocation {
    Surface Type = Surface::DevicePicker;
    SurfaceIntent Intent = SurfaceIntent::Show;
    Identifier128 Invocation;
    std::uint32_t Flags = SurfaceFlagNone;
    std::int32_t AnchorLeft = 0;
    std::int32_t AnchorTop = 0;
    std::int32_t AnchorRight = 0;
    std::int32_t AnchorBottom = 0;
    std::uint32_t Dpi = 0;

    bool operator==(SurfaceInvocation const&) const = default;
};

struct SurfaceFailure {
    SurfaceInvocation Invocation;
    std::uint32_t Error = ERROR_GEN_FAILURE;
    bool Retryable = false;

    bool operator==(SurfaceFailure const&) const = default;
};

struct SettingMutation {
    SettingKey Key = SettingKey::Language;
    SettingValueKind ValueKind = SettingValueKind::None;
    std::uint64_t ExpectedSettingsRevision = 0;
    std::wstring DeviceId;
    bool BooleanValue = false;
    std::int64_t IntegerValue = 0;
    std::wstring StringValue;
    WindowBounds BoundsValue;
};

struct CommandResult {
    ActionStatus Status = ActionStatus::Failed;
    StateRevisions Revisions;
    std::wstring Message;
};

enum class IoStatus {
    Success,
    Timeout,
    Cancelled,
    Closed,
    InvalidData,
    DuplicateRequestPending,
    DuplicateRequestReplayed,
    DuplicateRequestResultUnavailable,
    DuplicateRequestExpired,
    EpochRotationRequired,
    Failed
};
enum class ReplayDisposition {
    NewRequest,
    DuplicatePending,
    DuplicateCompleted,
    DuplicateResultUnavailable,
    PendingExpired,
    Conflict,
    CapacityExceeded,
    InternalFailure
};
enum class ReplayCompletion { Stored, AlreadyStored, NotTracked, Conflict, CapacityExceeded, InternalFailure };
struct ReplayObservation {
    ReplayDisposition Disposition = ReplayDisposition::Conflict;
    std::optional<SecureByteBuffer> CachedResult;
};
struct ReplayCacheLimits {
    std::size_t LedgerEntries = c_maxReplayLedgerEntries;
    std::size_t ResultEntries = c_maxReplayResultEntries;
    std::size_t ResultBytes = c_maxReplayResultBytes;
    std::uint64_t PendingMilliseconds = c_maxPendingRequestMilliseconds;
};
enum class SessionRole { Server, Client };
enum class FrameDirection { Inbound, Outbound };
enum class SessionPhase { Initial, ChallengeExchanged, AuthenticationExchanged, Active, Closed };

class SequenceValidator {
public:
    [[nodiscard]] bool Accept(std::uint64_t sequence) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::uint64_t LastAccepted() const noexcept { return m_lastAccepted; }

private:
    std::uint64_t m_lastAccepted = 0;
};

class SessionStateMachine {
public:
    SessionStateMachine(SessionRole role,
                        HostEpoch epoch,
                        ConnectionId connection,
                        SessionHandshakeContext handshake) noexcept;
    ~SessionStateMachine() noexcept;

    [[nodiscard]] bool ValidateAndAdvance(Frame const& frame, FrameDirection direction) noexcept;
    void Close() noexcept;
    [[nodiscard]] SessionPhase Phase() const noexcept;
    [[nodiscard]] std::uint64_t NegotiatedCapabilities() const noexcept;
    [[nodiscard]] std::optional<SessionCapability> EstablishedCapability() const noexcept;

private:
    friend class SessionEndpoint;
    [[nodiscard]] bool ValidateAndAdvanceCore(Frame const& frame, FrameDirection direction) noexcept;
    void ClearSecretsLocked() noexcept;

    SessionRole m_role;
    HostEpoch m_epoch;
    ConnectionId m_connection;
    SessionHandshakeContext m_handshake;
    mutable std::mutex m_mutex;
    SessionPhase m_phase = SessionPhase::Initial;
    std::uint64_t m_lastInboundSequence = 0;
    std::uint64_t m_lastOutboundSequence = 0;
    Secret256 m_clientNonce;
    std::uint64_t m_offeredCapabilities = 0;
    std::uint64_t m_negotiatedCapabilities = 0;
    std::optional<SessionCapability> m_establishedCapability;
};

class RequestReplayCache {
public:
    using TickSource = std::uint64_t (*)() noexcept;

    explicit RequestReplayCache(HostEpoch const& epoch,
                                ReplayCacheLimits limits = {},
                                TickSource tickSource = nullptr) noexcept;

    [[nodiscard]] ReplayObservation Observe(Frame const& request) noexcept;
    [[nodiscard]] ReplayCompletion Complete(CorrelationId const& correlation,
                                            SecureByteBuffer const& resultPayload) noexcept;
    void Abandon(CorrelationId const& correlation) noexcept;
    [[nodiscard]] bool MatchesEpoch(HostEpoch const& epoch) const noexcept;

private:
    friend class SessionEndpoint;
    RequestReplayCache(HostEpoch const& epoch,
                       bool retainCompleted,
                       ReplayCacheLimits limits,
                       TickSource tickSource) noexcept;
    struct CorrelationHash {
        [[nodiscard]] std::size_t operator()(CorrelationId const& value) const noexcept;
    };
    struct Entry {
        MessageKind Kind = MessageKind::ProtocolError;
        bool ReplayProtected = false;
        bool Completed = false;
        bool Expired = false;
        std::array<std::byte, 32> RequestDigest{};
        std::optional<SecureByteBuffer> ResultPayload;
        std::uint64_t CreatedAt = 0;
    };

    [[nodiscard]] bool MakeRoomLocked(std::size_t bytes) noexcept;
    [[nodiscard]] ReplayObservation ObserveValidated(Frame const& request) noexcept;
    [[nodiscard]] ReplayObservation ObserveCore(Frame const& request, bool syntaxValidated) noexcept;
    [[nodiscard]] ReplayCompletion CompleteValidated(CorrelationId const& correlation,
                                                     SecureByteBuffer const& resultPayload) noexcept;
    [[nodiscard]] ReplayCompletion CompleteCore(CorrelationId const& correlation,
                                                SecureByteBuffer const& resultPayload,
                                                bool syntaxValidated) noexcept;

    HostEpoch m_epoch;
    mutable std::mutex m_mutex;
    std::unordered_map<CorrelationId, Entry, CorrelationHash> m_entries;
    std::deque<CorrelationId> m_completedResults;
    std::size_t m_resultBytes = 0;
    std::size_t m_resultCount = 0;
    bool m_retainCompleted = true;
    ReplayCacheLimits m_limits;
    TickSource m_tickSource = nullptr;
};

class SessionEndpoint {
public:
    class UniquePipeHandle {
    public:
        explicit UniquePipeHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : m_value(value) {}
        ~UniquePipeHandle() noexcept;
        UniquePipeHandle(UniquePipeHandle const&) = delete;
        UniquePipeHandle& operator=(UniquePipeHandle const&) = delete;
        UniquePipeHandle(UniquePipeHandle&& other) noexcept;
        UniquePipeHandle& operator=(UniquePipeHandle&& other) noexcept;
        [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
        [[nodiscard]] HANDLE Release() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }

    private:
        HANDLE m_value = INVALID_HANDLE_VALUE;
    };

    [[nodiscard]] static std::unique_ptr<SessionEndpoint>
    CreateBound(SessionRole role,
                UniquePipeHandle connectedPipe,
                HANDLE expectedPeerProcess,
                ProcessBinding const& expectedPeer,
                HostEpoch const& epoch,
                ConnectionId const& connection,
                SessionHandshakeContext handshake,
                std::shared_ptr<RequestReplayCache> replayCache = {}) noexcept;
    ~SessionEndpoint() noexcept;

    SessionEndpoint(SessionEndpoint const&) = delete;
    SessionEndpoint& operator=(SessionEndpoint const&) = delete;

    [[nodiscard]] IoStatus Send(OutboundMessage message, HANDLE stopEvent, std::uint64_t deadline) noexcept;
    [[nodiscard]] IoStatus Receive(Frame& frame, HANDLE stopEvent, std::uint64_t deadline) noexcept;
    void Close() noexcept;
    [[nodiscard]] SessionPhase Phase() const noexcept;
    [[nodiscard]] std::uint64_t NegotiatedCapabilities() const noexcept;
    [[nodiscard]] std::optional<SessionCapability> EstablishedCapability() const noexcept;

private:
    SessionEndpoint(SessionRole role,
                    HANDLE ownedPipe,
                    HostEpoch const& epoch,
                    ConnectionId const& connection,
                    SessionHandshakeContext handshake,
                    std::shared_ptr<RequestReplayCache> replayCache) noexcept;
    [[nodiscard]] IoStatus
    SendCore(OutboundMessage message, HANDLE stopEvent, std::uint64_t deadline, bool validatedReplay) noexcept;
    void SignalTerminalFailureFromIo() noexcept;

    SessionRole m_role;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HostEpoch m_epoch;
    ConnectionId m_connection;
    SessionStateMachine m_state;
    std::shared_ptr<RequestReplayCache> m_replayCache;
    std::mutex m_readMutex;
    std::mutex m_writeMutex;
    std::mutex m_closeMutex;
    std::atomic_bool m_closing = false;
    std::uint64_t m_nextOutboundSequence = 1;
    HANDLE m_shutdownEvent = nullptr;
    HANDLE m_readEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
};

[[nodiscard]] std::optional<Identifier128> GenerateIdentifier() noexcept;
[[nodiscard]] std::optional<Secret256> GenerateSecret() noexcept;
[[nodiscard]] std::uint64_t NegotiateCapabilities(std::uint64_t clientCapabilities,
                                                  std::uint64_t serverCapabilities) noexcept;
[[nodiscard]] std::optional<std::wstring> PipeName(LaunchId const& launch) noexcept;
[[nodiscard]] std::optional<std::wstring> EncodeUiLaunchBootstrap(UiLaunchBootstrap const& bootstrap) noexcept;
[[nodiscard]] std::optional<UiLaunchBootstrap> DecodeUiLaunchBootstrap(std::wstring_view token) noexcept;
[[nodiscard]] std::optional<ProcessBinding> CaptureProcessBinding(HANDLE process,
                                                                  std::wstring_view executablePath) noexcept;
[[nodiscard]] bool
IsExpectedTrustedPipeClient(HANDLE pipe, HANDLE expectedProcess, ProcessBinding const& expected) noexcept;
[[nodiscard]] bool
IsExpectedTrustedPipeServer(HANDLE pipe, HANDLE expectedProcess, ProcessBinding const& expected) noexcept;

[[nodiscard]] bool IsKnownMessageKind(std::uint32_t value) noexcept;
[[nodiscard]] bool IsFrameValid(Frame const& frame) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeFrame(Frame const& frame);
[[nodiscard]] std::optional<Frame> DecodeFrame(std::span<std::byte const> wireBytes) noexcept;

[[nodiscard]] std::optional<SecureByteBuffer> EncodeServerChallenge(ServerChallenge const& value);
[[nodiscard]] std::optional<ServerChallenge> DecodeServerChallenge(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeClientAuthenticate(ClientAuthenticate const& value);
[[nodiscard]] std::optional<ClientAuthenticate> DecodeClientAuthenticate(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeReconnectAuthenticate(ReconnectAuthenticate const& value);
[[nodiscard]] std::optional<ReconnectAuthenticate>
DecodeReconnectAuthenticate(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeServerAccepted(ServerAccepted const& value);
[[nodiscard]] std::optional<ServerAccepted> DecodeServerAccepted(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<Secret256> ComputeReconnectProof(SessionCapability const& capability,
                                                             HostEpoch const& epoch,
                                                             LaunchId const& launch,
                                                             ConnectionId const& connection,
                                                             Secret256 const& serverNonce,
                                                             Secret256 const& clientNonce,
                                                             DWORD serverProcessId,
                                                             DWORD clientProcessId,
                                                             std::uint64_t capabilities) noexcept;

[[nodiscard]] std::optional<SecureByteBuffer> EncodeRevisionNotice(RevisionNotice const& notice);
[[nodiscard]] std::optional<RevisionNotice> DecodeRevisionNotice(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeSurfaceInvocation(SurfaceInvocation const& invocation);
[[nodiscard]] std::optional<SurfaceInvocation> DecodeSurfaceInvocation(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeSurfaceFailure(SurfaceFailure const& failure);
[[nodiscard]] std::optional<SurfaceFailure> DecodeSurfaceFailure(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeAppAction(AppAction action);
[[nodiscard]] std::optional<AppAction> DecodeAppAction(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeSnapshot(Snapshot const& snapshot);
[[nodiscard]] std::optional<Snapshot> DecodeSnapshot(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeDeviceConnectionRequest(DeviceConnectionRequest const& request);
[[nodiscard]] std::optional<DeviceConnectionRequest>
DecodeDeviceConnectionRequest(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeSettingMutation(SettingMutation const& mutation);
[[nodiscard]] std::optional<SettingMutation> DecodeSettingMutation(std::span<std::byte const> payload) noexcept;
[[nodiscard]] std::optional<SecureByteBuffer> EncodeCommandResult(CommandResult const& result);
[[nodiscard]] std::optional<CommandResult> DecodeCommandResult(std::span<std::byte const> payload) noexcept;

} // namespace apc::core_ui
