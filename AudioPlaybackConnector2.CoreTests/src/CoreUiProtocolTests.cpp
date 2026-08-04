#include <ipc/CoreUiProtocol.hpp>
#include <control/CommandProtocol.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
std::atomic_uint64_t g_replayTick = 0;

std::uint64_t ReplayTick() noexcept {
    return g_replayTick.load(std::memory_order_relaxed);
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
    ~ScopedHandle() {
        if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value);
    }
    ScopedHandle(ScopedHandle const&) = delete;
    ScopedHandle& operator=(ScopedHandle const&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
    [[nodiscard]] HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
    [[nodiscard]] explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_value = nullptr;
};

class ScopedOverlappedOperation {
public:
    ScopedOverlappedOperation(HANDLE handle, OVERLAPPED& operation, bool pending) noexcept
        : m_handle(handle), m_operation(&operation), m_pending(pending) {}
    ~ScopedOverlappedOperation() noexcept { CancelAndDrain(); }
    ScopedOverlappedOperation(ScopedOverlappedOperation const&) = delete;
    ScopedOverlappedOperation& operator=(ScopedOverlappedOperation const&) = delete;

    void Complete() noexcept { m_pending = false; }
    void CancelAndDrain() noexcept {
        if (!m_pending) return;
        CancelIoEx(m_handle, m_operation);
        DWORD ignored = 0;
        static_cast<void>(GetOverlappedResult(m_handle, m_operation, &ignored, TRUE));
        m_pending = false;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    OVERLAPPED* m_operation = nullptr;
    bool m_pending = false;
};

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

apc::core_ui::Identifier128 Identifier(std::uint8_t seed) {
    apc::core_ui::Identifier128 value;
    for (std::size_t index = 0; index < value.Bytes.size(); ++index) {
        value.Bytes[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    }
    return value;
}

apc::core_ui::Secret256 Secret(std::uint8_t seed) {
    apc::core_ui::Secret256 value;
    for (std::size_t index = 0; index < value.Bytes.size(); ++index) {
        value.Bytes[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    }
    return value;
}

std::optional<std::wstring> CurrentExecutablePath() {
    std::wstring path(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::nullopt;
    path.resize(length);
    return path;
}

std::uint64_t FileTimeValue(FILETIME value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

apc::core_ui::Snapshot MakeSnapshot() {
    using namespace apc::core_ui;
    Snapshot snapshot;
    snapshot.Revisions = {9, 3, 4, 5, 7};
    snapshot.Domains = CapabilityDevicePicker | CapabilitySettings | CapabilityAppearancePolicy | CapabilityUpdates;
    snapshot.Flags = SnapshotFlagInventoryFresh | SnapshotFlagHasCachedInventory;
    snapshot.Appearance = AppearanceFlagAllowMica | AppearanceFlagAllowAcrylic | AppearanceFlagTransparencyEnabled;
    snapshot.ConnectedDeviceCount = 1;
    snapshot.Inventory = InventoryStatus::Ready;
    snapshot.KnownDeviceTotal = 2;
    snapshot.DeviceTotal = 2;
    snapshot.Settings.Flags =
        SettingsFlagShowNotifications | SettingsFlagHasWindowBounds | SettingsFlagSpecificDefaultDevice;
    snapshot.Settings.StartupState = StartupTaskState::Enabled;
    snapshot.Settings.Language = L"de";
    snapshot.Settings.DefaultDeviceId = L"device-1";
    snapshot.Settings.Bounds = {100, 120, 900, 700, 144};
    snapshot.Update = {UpdateStatus::Available, L"2.1.0"};
    snapshot.KnownDevices = {
        {L"device-1", L"Kopfh\x00F6rer", L"Schreibtisch", KnownDeviceFlagReconnectOnConnectionLoss},
        {L"offline-device", L"Gespeichert", L"Reise", KnownDeviceFlagConnectOnStartup},
    };
    snapshot.Devices = {
        {L"device-1", L"Kopfh\x00F6rer", L"Schreibtisch", L"Schreibtisch", DeviceFlagConnected},
        {L"device-2", L"Speaker", L"", L"Speaker", DeviceFlagNone},
    };
    return snapshot;
}

void TestRandomIdentifiersAndPipeNames() {
    using namespace apc::core_ui;
    auto first = GenerateIdentifier();
    auto second = GenerateIdentifier();
    auto secret = GenerateSecret();
    Check(first && !first->Empty(), "generated launch identifiers must be non-empty");
    Check(second && !second->Empty(), "a second generated launch identifier must be non-empty");
    Check(secret && !secret->Empty(), "generated session capabilities must be non-empty");
    Check(first && second && !(*first == *second), "independent launch identifiers must differ");
    Check(first && PipeName(*first).has_value(), "a valid launch identifier must produce a scoped pipe name");
    Check(!PipeName({}), "an empty launch identifier must never produce a pipe endpoint");
    if (first && second) {
        UiLaunchBootstrap bootstrap{
            *first, *second, Identifier(80), 0x1234, 100, 10'000, CapabilityDevicePicker | CapabilitySettings};
        auto token = EncodeUiLaunchBootstrap(bootstrap);
        auto decoded = token ? DecodeUiLaunchBootstrap(*token) : std::nullopt;
        Check(decoded && decoded->Launch == bootstrap.Launch && decoded->Epoch == bootstrap.Epoch &&
                  decoded->Connection == bootstrap.Connection &&
                  decoded->InheritedCoreProcessHandle == bootstrap.InheritedCoreProcessHandle &&
                  decoded->CoreProcessId == bootstrap.CoreProcessId &&
                  decoded->CoreCreationTime == bootstrap.CoreCreationTime &&
                  decoded->SupportedCapabilities == bootstrap.SupportedCapabilities,
              "the inherited-handle launch bootstrap must round-trip canonically");
        if (token) {
            auto invalid = *token;
            invalid.back() = L'z';
            Check(!DecodeUiLaunchBootstrap(invalid), "invalid bootstrap tokens must fail closed");
        }
    }
    if (secret) {
        secret->Clear();
        Check(secret->Empty(), "clearing a capability must erase every byte");
    }
}

void TestSequenceValidation() {
    apc::core_ui::SequenceValidator validator;
    Check(!validator.Accept(0), "sequence zero must be rejected");
    Check(!validator.Accept(2), "a sequence gap at session start must be rejected");
    Check(validator.Accept(1), "the first frame sequence must be one");
    Check(!validator.Accept(1), "a replayed sequence must be rejected");
    Check(!validator.Accept(3), "a sequence gap must be rejected");
    Check(validator.Accept(2), "the next contiguous sequence must be accepted");
    validator.Reset();
    Check(validator.LastAccepted() == 0 && validator.Accept(1), "reset must begin a fresh connection sequence");
}

void TestHandshakeAndReconnectProofs() {
    using namespace apc::core_ui;
    ServerChallenge challenge{Identifier(1), Secret(20), 120, 121, 10'000, 11'000};
    auto challengeBytes = EncodeServerChallenge(challenge);
    auto decodedChallenge = challengeBytes ? DecodeServerChallenge(*challengeBytes) : std::nullopt;
    Check(decodedChallenge && decodedChallenge->Launch == challenge.Launch &&
              decodedChallenge->ServerNonce == challenge.ServerNonce &&
              decodedChallenge->ServerCreationTime == challenge.ServerCreationTime,
          "server challenges must round-trip without native-struct layout assumptions");
    if (challengeBytes) {
        auto trailing = *challengeBytes;
        trailing.push_back(std::byte{});
        Check(!DecodeServerChallenge(trailing), "handshake payloads with trailing bytes must be rejected");
    }

    ClientAuthenticate authentication{challenge.Launch,
                                      challenge.ServerNonce,
                                      Secret(60),
                                      CapabilityDevicePicker | CapabilitySettings | CapabilityReconnect};
    auto authBytes = EncodeClientAuthenticate(authentication);
    auto decodedAuth = authBytes ? DecodeClientAuthenticate(*authBytes) : std::nullopt;
    Check(decodedAuth && decodedAuth->ClientNonce == authentication.ClientNonce &&
              decodedAuth->Capabilities == authentication.Capabilities,
          "client authentication payloads must round-trip");
    auto unknownCapabilities = authentication;
    unknownCapabilities.Capabilities |= 1ULL << 63;
    Check(!EncodeClientAuthenticate(unknownCapabilities), "unknown capability bits must fail closed");
    Check(NegotiateCapabilities(authentication.Capabilities, CapabilitySettings | CapabilityUpdates) ==
              CapabilitySettings,
          "capability negotiation must retain only the supported intersection");

    const auto capability = Secret(100);
    const auto epoch = Identifier(30);
    const auto connection = Identifier(50);
    const auto proofCapabilities = authentication.Capabilities;
    auto firstProof = ComputeReconnectProof(capability,
                                            epoch,
                                            challenge.Launch,
                                            connection,
                                            challenge.ServerNonce,
                                            authentication.ClientNonce,
                                            challenge.ServerProcessId,
                                            challenge.ClientProcessId,
                                            proofCapabilities);
    auto replayChanged = ComputeReconnectProof(capability,
                                               epoch,
                                               challenge.Launch,
                                               Identifier(51),
                                               challenge.ServerNonce,
                                               authentication.ClientNonce,
                                               challenge.ServerProcessId,
                                               challenge.ClientProcessId,
                                               proofCapabilities);
    Check(firstProof && !firstProof->Empty(), "a complete reconnect transcript must produce an HMAC proof");
    Check(firstProof && replayChanged && !(*firstProof == *replayChanged),
          "a reconnect proof must be bound to its connection identifier");
    constexpr std::array<std::uint8_t, 32> expectedProof = {
        0xec, 0xd7, 0xb7, 0x69, 0x79, 0x19, 0xe4, 0x45, 0xa8, 0x10, 0xbe, 0x97, 0x45, 0x50, 0xdf, 0xc0,
        0x41, 0x4a, 0xc8, 0xf3, 0x75, 0x89, 0x1f, 0xef, 0x35, 0x25, 0x7d, 0xda, 0x3a, 0x56, 0x99, 0xe8};
    bool proofMatches = firstProof.has_value();
    for (std::size_t index = 0; proofMatches && index < expectedProof.size(); ++index) {
        proofMatches = std::to_integer<std::uint8_t>(firstProof->Bytes[index]) == expectedProof[index];
    }
    Check(proofMatches, "reconnect proof must match the fixed HMAC-SHA-256 test vector");

    const auto differs = [&](SessionCapability const& changedCapability,
                             HostEpoch const& changedEpoch,
                             LaunchId const& changedLaunch,
                             ConnectionId const& changedConnection,
                             Secret256 const& changedServerNonce,
                             Secret256 const& changedClientNonce,
                             DWORD changedServerProcessId,
                             DWORD changedClientProcessId,
                             std::uint64_t changedCapabilities) {
        auto proof = ComputeReconnectProof(changedCapability,
                                           changedEpoch,
                                           changedLaunch,
                                           changedConnection,
                                           changedServerNonce,
                                           changedClientNonce,
                                           changedServerProcessId,
                                           changedClientProcessId,
                                           changedCapabilities);
        return firstProof && proof && !(*firstProof == *proof);
    };
    Check(differs(Secret(101),
                  epoch,
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  120,
                  121,
                  proofCapabilities),
          "reconnect proof must be keyed by the session capability");
    Check(differs(capability,
                  Identifier(31),
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  120,
                  121,
                  proofCapabilities),
          "reconnect proof must bind the host epoch");
    Check(differs(capability,
                  epoch,
                  Identifier(2),
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  120,
                  121,
                  proofCapabilities),
          "reconnect proof must bind the launch identifier");
    Check(differs(capability,
                  epoch,
                  challenge.Launch,
                  connection,
                  Secret(21),
                  authentication.ClientNonce,
                  120,
                  121,
                  proofCapabilities),
          "reconnect proof must bind the server challenge");
    Check(differs(capability,
                  epoch,
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  Secret(61),
                  120,
                  121,
                  proofCapabilities),
          "reconnect proof must bind the client nonce");
    Check(differs(capability,
                  epoch,
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  122,
                  121,
                  proofCapabilities),
          "reconnect proof must bind the server PID");
    Check(differs(capability,
                  epoch,
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  120,
                  123,
                  proofCapabilities),
          "reconnect proof must bind the client PID");
    Check(differs(capability,
                  epoch,
                  challenge.Launch,
                  connection,
                  challenge.ServerNonce,
                  authentication.ClientNonce,
                  120,
                  121,
                  CapabilityDevicePicker | CapabilitySettings),
          "reconnect proof must bind the offered capabilities");
}

void TestSnapshotWireContract() {
    using namespace apc::core_ui;
    auto expected = MakeSnapshot();
    auto bytes = EncodeSnapshot(expected);
    auto decoded = bytes ? DecodeSnapshot(*bytes) : std::nullopt;
    Check(decoded && *decoded == expected, "full device/settings snapshots must round-trip losslessly");

    if (bytes) {
        for (std::size_t length = 0; length < bytes->size(); ++length) {
            if (DecodeSnapshot(std::span(bytes->data(), length))) {
                Check(false, "every truncated snapshot prefix must be rejected");
                break;
            }
        }
        auto trailing = *bytes;
        trailing.push_back(std::byte{});
        Check(!DecodeSnapshot(trailing), "snapshots with trailing bytes must be rejected");
    }

    auto duplicate = expected;
    duplicate.Devices.push_back(duplicate.Devices.front());
    duplicate.ConnectedDeviceCount = 2;
    Check(!EncodeSnapshot(duplicate), "duplicate device identifiers must be rejected");
    auto wrongConnectedCount = expected;
    wrongConnectedCount.ConnectedDeviceCount = 0;
    Check(!EncodeSnapshot(wrongConnectedCount), "connected-device counts must match device flags");
    auto invalidFlags = expected;
    invalidFlags.Appearance |= 1U << 31;
    Check(!EncodeSnapshot(invalidFlags), "unknown appearance flags must be rejected");
    auto invalidDomains = expected;
    invalidDomains.Domains |= CapabilityReconnect;
    Check(!EncodeSnapshot(invalidDomains), "transport-only capabilities must not appear as snapshot domains");
    auto failedInventory = expected;
    failedInventory.Flags &= ~SnapshotFlagInventoryFresh;
    failedInventory.Inventory = InventoryStatus::Failed;
    failedInventory.InventoryError = ERROR_DEVICE_NOT_AVAILABLE;
    Check(EncodeSnapshot(failedInventory).has_value(),
          "a failed refresh must preserve a usable cached inventory and stable error code");
    auto contradictoryInventory = failedInventory;
    contradictoryInventory.InventoryError = ERROR_SUCCESS;
    Check(!EncodeSnapshot(contradictoryInventory), "failed inventory states require a non-success error code");
    auto missingDefault = expected;
    missingDefault.Settings.DefaultDeviceId.clear();
    Check(!EncodeSnapshot(missingDefault), "specific-default mode must include exactly one default device id");
    auto staleBounds = expected;
    staleBounds.Settings.Flags &= ~SettingsFlagHasWindowBounds;
    Check(!EncodeSnapshot(staleBounds), "window bounds without the presence flag must be rejected as non-canonical");

    auto maximum = expected;
    maximum.ConnectedDeviceCount = 0;
    maximum.KnownDevices.clear();
    maximum.Devices.clear();
    const std::wstring maximumName(c_maxDeviceNameCharacters, L'n');
    const std::wstring maximumAlias(c_maxDeviceAliasCharacters, L'a');
    for (std::uint32_t index = 0; index < c_maxDeviceCount; ++index) {
        std::wstring id = std::to_wstring(index) + L":";
        id.resize(c_maxDeviceIdCharacters, L'x');
        maximum.KnownDevices.push_back({id,
                                        maximumName,
                                        maximumAlias,
                                        KnownDeviceFlagConnectOnStartup | KnownDeviceFlagReconnectOnConnectionLoss});
        maximum.Devices.push_back({id, maximumName, maximumAlias, maximumName, DeviceFlagNone});
    }
    maximum.KnownDeviceTotal = static_cast<std::uint32_t>(maximum.KnownDevices.size());
    maximum.DeviceTotal = static_cast<std::uint32_t>(maximum.Devices.size());
    auto maximumBytes = EncodeSnapshot(maximum);
    Check(maximumBytes && maximumBytes->size() <= c_maxPayloadBytes,
          "the documented maximum snapshot must always fit in one bounded frame");
    auto paged = maximum;
    paged.KnownDeviceOffset = c_maxDeviceCount;
    paged.KnownDeviceTotal = c_maxDeviceCount * 3;
    paged.DeviceOffset = c_maxDeviceCount;
    paged.DeviceTotal = c_maxDeviceCount * 3;
    Check(EncodeSnapshot(paged).has_value(), "large persisted device sets must be representable as bounded pages");
}

void TestActionWireContracts() {
    using namespace apc::core_ui;
    DeviceConnectionRequest connect{DeviceAction::SetConnected, 11, L"device-1", true};
    auto connectBytes = EncodeDeviceConnectionRequest(connect);
    auto decodedConnect = connectBytes ? DecodeDeviceConnectionRequest(*connectBytes) : std::nullopt;
    Check(decodedConnect && decodedConnect->DeviceId == connect.DeviceId && decodedConnect->ShouldBeConnected,
          "desired device connection state must round-trip");
    Check(!EncodeDeviceConnectionRequest({DeviceAction::SetConnected, 11, L"", true}),
          "single-device actions without an identifier must be rejected");
    DeviceConnectionRequest reconnectAll{DeviceAction::ReconnectAll, 11, L"", false};
    auto reconnectAllBytes = EncodeDeviceConnectionRequest(reconnectAll);
    Check(reconnectAllBytes && DecodeDeviceConnectionRequest(*reconnectAllBytes)->Type == DeviceAction::ReconnectAll,
          "explicit all-device actions must round-trip without toggle semantics");

    SettingMutation alias;
    alias.Key = SettingKey::DeviceAlias;
    alias.ValueKind = SettingValueKind::String;
    alias.ExpectedSettingsRevision = 14;
    alias.DeviceId = L"device-1";
    alias.StringValue = L"Studio";
    auto aliasBytes = EncodeSettingMutation(alias);
    auto decodedAlias = aliasBytes ? DecodeSettingMutation(*aliasBytes) : std::nullopt;
    Check(decodedAlias && decodedAlias->Key == alias.Key && decodedAlias->DeviceId == alias.DeviceId &&
              decodedAlias->StringValue == alias.StringValue &&
              decodedAlias->ExpectedSettingsRevision == alias.ExpectedSettingsRevision,
          "revision-checked device settings mutations must round-trip");

    auto wrongKind = alias;
    wrongKind.ValueKind = SettingValueKind::Boolean;
    Check(!EncodeSettingMutation(wrongKind), "a setting key with the wrong value type must be rejected");
    auto invalidUnicode = alias;
    invalidUnicode.StringValue = std::wstring(1, static_cast<wchar_t>(0xD800));
    Check(!EncodeSettingMutation(invalidUnicode), "unpaired UTF-16 surrogates must be rejected");

    SettingMutation language;
    language.Key = SettingKey::Language;
    language.ValueKind = SettingValueKind::String;
    language.StringValue = L"en";
    Check(EncodeSettingMutation(language).has_value(), "an explicitly supported language must be serializable");
    language.StringValue = L"de-DE";
    Check(!EncodeSettingMutation(language), "an unknown locale tag must be rejected instead of silently falling back");

    SettingMutation defaultDevice;
    defaultDevice.Key = SettingKey::DefaultDevice;
    defaultDevice.ValueKind = SettingValueKind::String;
    defaultDevice.ExpectedSettingsRevision = 15;
    defaultDevice.StringValue = L"device-2";
    auto defaultBytes = EncodeSettingMutation(defaultDevice);
    Check(defaultBytes && DecodeSettingMutation(*defaultBytes)->StringValue == L"device-2",
          "one atomic default-device mutation must select a specific device");
    defaultDevice.StringValue.clear();
    Check(EncodeSettingMutation(defaultDevice).has_value(),
          "the same atomic default-device mutation must select last-connected with an empty identifier");
    defaultDevice.ValueKind = SettingValueKind::Integer;
    defaultDevice.IntegerValue = 1;
    Check(!EncodeSettingMutation(defaultDevice),
          "a separate default-device mode mutation must not permit non-atomic persisted state");

    SettingMutation bounds;
    bounds.Key = SettingKey::SettingsWindowBounds;
    bounds.ValueKind = SettingValueKind::WindowBounds;
    bounds.BoundsValue = {10, 20, 800, 600, 96};
    Check(EncodeSettingMutation(bounds).has_value(), "valid settings-window bounds must be serializable");
    bounds.BoundsValue.Dpi = 0;
    Check(!EncodeSettingMutation(bounds), "invalid persisted DPI values must be rejected");

    StateRevisions revisions{12, 4, 5, 6, 7};
    RevisionNotice revisionNotice{CapabilityDevicePicker | CapabilitySettings | CapabilityAppearancePolicy, revisions};
    auto revisionBytes = EncodeRevisionNotice(revisionNotice);
    Check(revisionBytes && DecodeRevisionNotice(*revisionBytes) == revisionNotice,
          "coalesced state notifications must carry all domain revisions");
    Check(!EncodeRevisionNotice({}), "state revision zero must never be announced as current state");
    auto unauthorizedRevision = revisionNotice;
    unauthorizedRevision.Domains = CapabilityDevicePicker;
    Check(!EncodeRevisionNotice(unauthorizedRevision),
          "revision notices must zero fields outside their declared capability domains");
    SurfaceInvocation invocation{Surface::DevicePicker,
                                 SurfaceIntent::Toggle,
                                 Identifier(70),
                                 SurfaceFlagHasAnchor | SurfaceFlagUserInitiated,
                                 100,
                                 200,
                                 124,
                                 224,
                                 144};
    auto surfaceBytes = EncodeSurfaceInvocation(invocation);
    Check(surfaceBytes && DecodeSurfaceInvocation(*surfaceBytes) == invocation,
          "surface lifecycle messages must preserve invocation, tray anchor, and DPI");
    SurfaceFailure failure{invocation, ERROR_NOT_ENOUGH_MEMORY, true};
    auto failureBytes = EncodeSurfaceFailure(failure);
    Check(failureBytes && DecodeSurfaceFailure(*failureBytes) == failure,
          "surface creation failures must terminate an invocation with an actionable error");
    failure.Error = ERROR_SUCCESS;
    Check(!EncodeSurfaceFailure(failure), "a successful surface transition must not be encoded as a failure");
    invocation.AnchorRight = invocation.AnchorLeft;
    Check(!EncodeSurfaceInvocation(invocation), "invalid tray anchor rectangles must be rejected");
    auto updateAction = EncodeAppAction(AppAction::CheckForUpdates);
    Check(updateAction && DecodeAppAction(*updateAction) == AppAction::CheckForUpdates,
          "manual update checks must use an explicit host action");
}

void TestFrameContractAndStrictHeaderValidation() {
    using namespace apc::core_ui;
    auto snapshotBytes = EncodeSnapshot(MakeSnapshot());
    Check(snapshotBytes.has_value(), "frame tests require a valid snapshot payload");
    if (!snapshotBytes) return;

    Frame frame;
    frame.Kind = MessageKind::FullSnapshot;
    frame.Epoch = Identifier(1);
    frame.Connection = Identifier(40);
    frame.Sequence = 1;
    frame.Payload = *snapshotBytes;
    auto wire = EncodeFrame(frame);
    auto decoded = wire ? DecodeFrame(*wire) : std::nullopt;
    Check(decoded && decoded->Kind == frame.Kind && decoded->Epoch == frame.Epoch &&
              decoded->Connection == frame.Connection && decoded->Sequence == frame.Sequence &&
              decoded->Payload == frame.Payload,
          "frames must round-trip through explicit little-endian wire encoding");
    if (!wire) return;

    Check(wire->size() == c_wireHeaderBytes + frame.Payload.size(), "wire header length must remain stable");
    Check(std::to_integer<unsigned int>((*wire)[0]) == 0x41 && std::to_integer<unsigned int>((*wire)[1]) == 0x55,
          "the frame magic must be emitted explicitly in little-endian order");

    auto wrongMagic = *wire;
    wrongMagic[0] ^= std::byte{1};
    Check(!DecodeFrame(wrongMagic), "frames with the wrong magic must be rejected");
    auto newerMajor = *wire;
    newerMajor[4] = static_cast<std::byte>(c_protocolMajor + 1);
    Check(!DecodeFrame(newerMajor), "unknown protocol major versions must be rejected");
    auto reserved = *wire;
    reserved[10] = std::byte{1};
    Check(!DecodeFrame(reserved), "nonzero reserved header fields must be rejected");
    auto trailing = *wire;
    trailing.push_back(std::byte{});
    Check(!DecodeFrame(trailing), "frame byte counts must be exact");

    auto missingCorrelation = frame;
    missingCorrelation.Kind = MessageKind::RequestFullSnapshot;
    missingCorrelation.Payload.clear();
    Check(!EncodeFrame(missingCorrelation), "request frames must carry a non-empty correlation identifier");
    missingCorrelation.Correlation = Identifier(90);
    Check(EncodeFrame(missingCorrelation).has_value(), "correlated request frames must be accepted");
}

void TestMalformedPayloadCorpus() {
    using namespace apc::core_ui;
    std::uint32_t state = 0x91E10DA5;
    for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const std::size_t length = state % 320;
        std::vector<std::byte> bytes(length);
        for (auto& value : bytes) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            value = static_cast<std::byte>(state & 0xFF);
        }
        static_cast<void>(DecodeFrame(bytes));
        static_cast<void>(DecodeSnapshot(bytes));
        static_cast<void>(DecodeSettingMutation(bytes));
        static_cast<void>(DecodeServerChallenge(bytes));
        static_cast<void>(DecodeClientAuthenticate(bytes));
        static_cast<void>(DecodeReconnectAuthenticate(bytes));
    }
    Check(true, "deterministic malformed payload corpus must remain exception-safe");
}

void TestSessionStateMachine() {
    using namespace apc::core_ui;
    const auto epoch = Identifier(1);
    const auto connection = Identifier(20);
    ServerChallenge challenge{Identifier(2), Secret(10), 100, 101, 1'000, 2'000};
    SessionHandshakeContext serverContext{challenge, CapabilityDevicePicker | CapabilitySettings, std::nullopt};
    SessionStateMachine server(SessionRole::Server, epoch, connection, serverContext);

    Frame prematureRequest;
    prematureRequest.Kind = MessageKind::RequestFullSnapshot;
    prematureRequest.Epoch = epoch;
    prematureRequest.Connection = connection;
    prematureRequest.Sequence = 1;
    prematureRequest.Correlation = Identifier(40);
    Check(!server.ValidateAndAdvance(prematureRequest, FrameDirection::Inbound),
          "commands before authentication must be rejected without advancing state");

    Frame challengeFrame;
    challengeFrame.Kind = MessageKind::ServerChallenge;
    challengeFrame.Epoch = epoch;
    challengeFrame.Connection = connection;
    challengeFrame.Sequence = 1;
    challengeFrame.Payload = *EncodeServerChallenge(challenge);
    Check(server.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound) &&
              server.Phase() == SessionPhase::ChallengeExchanged,
          "the server must begin with exactly one outbound challenge");
    Check(!server.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound),
          "a replayed handshake sequence must be rejected");

    ClientAuthenticate auth{
        challenge.Launch, challenge.ServerNonce, Secret(50), CapabilityDevicePicker | CapabilitySettings};
    Frame authFrame;
    authFrame.Kind = MessageKind::ClientAuthenticate;
    authFrame.Epoch = epoch;
    authFrame.Connection = connection;
    authFrame.Sequence = 1;
    authFrame.Payload = *EncodeClientAuthenticate(auth);
    Check(server.ValidateAndAdvance(authFrame, FrameDirection::Inbound) &&
              server.Phase() == SessionPhase::AuthenticationExchanged,
          "only client authentication may follow the challenge");

    Frame acceptedFrame;
    acceptedFrame.Kind = MessageKind::ServerAccepted;
    acceptedFrame.Epoch = epoch;
    acceptedFrame.Connection = connection;
    acceptedFrame.Sequence = 2;
    acceptedFrame.Payload = *EncodeServerAccepted({{}, CapabilityDevicePicker | CapabilitySettings});
    Check(server.ValidateAndAdvance(acceptedFrame, FrameDirection::Outbound) && server.Phase() == SessionPhase::Active,
          "the authenticated server acceptance must activate the session");
    Check(server.NegotiatedCapabilities() == (CapabilityDevicePicker | CapabilitySettings) &&
              !server.EstablishedCapability(),
          "the active session must expose exactly the negotiated non-reconnect capabilities");

    auto wrongDirection = acceptedFrame;
    wrongDirection.Kind = MessageKind::FullSnapshot;
    wrongDirection.Sequence = 2;
    wrongDirection.Payload = *EncodeSnapshot(MakeSnapshot());
    Check(!server.ValidateAndAdvance(wrongDirection, FrameDirection::Inbound),
          "a client must never inject server-only snapshot messages");
    prematureRequest.Sequence = 2;
    Check(server.ValidateAndAdvance(prematureRequest, FrameDirection::Inbound),
          "the first authenticated client command must retain inbound sequence two");

    Frame unauthorizedUpdate{MessageKind::AppActionRequest,
                             0,
                             epoch,
                             connection,
                             3,
                             Identifier(41),
                             *EncodeAppAction(AppAction::CheckForUpdates)};
    Check(!server.ValidateAndAdvance(unauthorizedUpdate, FrameDirection::Inbound),
          "active messages must be rejected when their capability was not negotiated");
    SettingMutation allowedSetting;
    allowedSetting.Key = SettingKey::ShowNotifications;
    allowedSetting.ValueKind = SettingValueKind::Boolean;
    allowedSetting.ExpectedSettingsRevision = 5;
    Frame allowedSettingFrame{
        MessageKind::SetSetting, 0, epoch, connection, 3, Identifier(42), *EncodeSettingMutation(allowedSetting)};
    Check(server.ValidateAndAdvance(allowedSettingFrame, FrameDirection::Inbound),
          "messages covered by the negotiated settings capability must remain valid");

    authFrame.Sequence = 4;
    Check(!server.ValidateAndAdvance(authFrame, FrameDirection::Inbound),
          "an active session must reject a repeated authentication handshake");
    challengeFrame.Sequence = 3;
    Check(!server.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound),
          "an active session must reject a repeated server challenge");

    auto wrongEpoch = prematureRequest;
    wrongEpoch.Sequence = 4;
    wrongEpoch.Epoch = Identifier(3);
    Check(!server.ValidateAndAdvance(wrongEpoch, FrameDirection::Inbound),
          "frames from a different host epoch must fail closed");

    auto clientChallenge = challenge;
    clientChallenge.ServerNonce.Clear();
    SessionHandshakeContext clientContext{clientChallenge, CapabilityDevicePicker | CapabilitySettings, std::nullopt};
    SessionStateMachine client(SessionRole::Client, epoch, connection, clientContext);
    auto clientChallengeFrame = challengeFrame;
    clientChallengeFrame.Sequence = 1;
    Check(client.ValidateAndAdvance(clientChallengeFrame, FrameDirection::Inbound),
          "a bound client must learn the fresh nonce from a challenge with matching process metadata");
    auto wrongChallenge = challenge;
    wrongChallenge.ClientCreationTime++;
    auto wrongChallengeFrame = clientChallengeFrame;
    wrongChallengeFrame.Payload = *EncodeServerChallenge(wrongChallenge);
    SessionStateMachine rejectingClient(SessionRole::Client, epoch, connection, std::move(clientContext));
    Check(!rejectingClient.ValidateAndAdvance(wrongChallengeFrame, FrameDirection::Inbound),
          "a client must reject challenge metadata that does not match its process bootstrap");
}

void TestReconnectSessionStateMachine() {
    using namespace apc::core_ui;
    const auto epoch = Identifier(70);
    const auto connection = Identifier(90);
    ServerChallenge challenge{Identifier(10), Secret(30), 200, 201, 3'000, 4'000};
    const auto oldCapability = Secret(100);
    constexpr auto capabilities = CapabilityDevicePicker | CapabilityReconnect;
    SessionHandshakeContext serverContext{challenge, capabilities, oldCapability};
    SessionStateMachine server(SessionRole::Server, epoch, connection, serverContext);

    Frame challengeFrame{MessageKind::ServerChallenge, 0, epoch, connection, 1, {}, *EncodeServerChallenge(challenge)};
    Check(server.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound),
          "a reconnect session must begin with a fresh server challenge");

    const auto clientNonce = Secret(60);
    auto proof = ComputeReconnectProof(oldCapability,
                                       epoch,
                                       challenge.Launch,
                                       connection,
                                       challenge.ServerNonce,
                                       clientNonce,
                                       challenge.ServerProcessId,
                                       challenge.ClientProcessId,
                                       capabilities);
    Check(proof.has_value(), "a reconnect transcript must produce its proof");
    if (!proof) return;
    ReconnectAuthenticate authentication{challenge.Launch, challenge.ServerNonce, clientNonce, capabilities, *proof};
    Frame authenticationFrame{
        MessageKind::ClientReconnect, 0, epoch, connection, 1, {}, *EncodeReconnectAuthenticate(authentication)};
    Check(server.ValidateAndAdvance(authenticationFrame, FrameDirection::Inbound),
          "the server must accept a transcript-bound reconnect proof");

    const auto rotatedCapability = Secret(140);
    Frame acceptedFrame{MessageKind::ServerAccepted,
                        0,
                        epoch,
                        connection,
                        2,
                        {},
                        *EncodeServerAccepted({rotatedCapability, capabilities})};
    Check(server.ValidateAndAdvance(acceptedFrame, FrameDirection::Outbound) &&
              server.NegotiatedCapabilities() == capabilities &&
              server.EstablishedCapability() == std::optional<SessionCapability>(rotatedCapability),
          "a reconnect session must rotate and expose only its new capability");

    SessionStateMachine rejectingServer(SessionRole::Server, epoch, connection, serverContext);
    Check(rejectingServer.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound),
          "the rejecting reconnect fixture must accept its challenge");
    auto invalidAuthentication = authentication;
    invalidAuthentication.Proof.Bytes.front() ^= std::byte{1};
    auto invalidAuthenticationFrame = authenticationFrame;
    invalidAuthenticationFrame.Payload = *EncodeReconnectAuthenticate(invalidAuthentication);
    Check(!rejectingServer.ValidateAndAdvance(invalidAuthenticationFrame, FrameDirection::Inbound),
          "a reconnect proof mutation must be rejected before activation");

    SessionStateMachine rotationRejectingServer(SessionRole::Server, epoch, connection, serverContext);
    Check(rotationRejectingServer.ValidateAndAdvance(challengeFrame, FrameDirection::Outbound) &&
              rotationRejectingServer.ValidateAndAdvance(authenticationFrame, FrameDirection::Inbound),
          "the rotation fixture must authenticate before acceptance");
    auto reusedCapabilityFrame = acceptedFrame;
    reusedCapabilityFrame.Payload = *EncodeServerAccepted({oldCapability, capabilities});
    Check(!rotationRejectingServer.ValidateAndAdvance(reusedCapabilityFrame, FrameDirection::Outbound),
          "a reconnect session must reject reuse of its old capability");

    auto clientChallenge = challenge;
    clientChallenge.ServerNonce.Clear();
    SessionHandshakeContext clientContext{clientChallenge, capabilities, oldCapability};
    SessionStateMachine client(SessionRole::Client, epoch, connection, std::move(clientContext));
    Check(client.ValidateAndAdvance(challengeFrame, FrameDirection::Inbound) &&
              client.ValidateAndAdvance(authenticationFrame, FrameDirection::Outbound),
          "the client must learn the nonce before validating its outbound reconnect proof");
}

void TestRequestReplayCache() {
    using namespace apc::core_ui;
    const auto epoch = Identifier(1);
    RequestReplayCache cache(epoch);
    Frame request{
        MessageKind::SetSetting,
        0,
        epoch,
        Identifier(30),
        2,
        Identifier(60),
        *EncodeSettingMutation({SettingKey::ShowNotifications, SettingValueKind::Boolean, 5, L"", true, 0, L"", {}})};
    Check(cache.Observe(request).Disposition == ReplayDisposition::NewRequest,
          "the first correlation must be admitted exactly once");
    Check(cache.Observe(request).Disposition == ReplayDisposition::DuplicatePending,
          "a retry must not execute while its original request is pending");
    auto conflicting = request;
    conflicting.Payload =
        *EncodeSettingMutation({SettingKey::ShowNotifications, SettingValueKind::Boolean, 5, L"", false, 0, L"", {}});
    Check(cache.Observe(conflicting).Disposition == ReplayDisposition::Conflict,
          "one correlation must never identify two different requests");
    auto result = EncodeCommandResult({ActionStatus::Success, {9, 2, 3, 4, 5}, L""});
    Check(result && cache.Complete(request.Correlation, *result) == ReplayCompletion::Stored,
          "a completed request must persist its canonical result");
    const auto completed = cache.Observe(request);
    Check(completed.Disposition == ReplayDisposition::DuplicateCompleted && completed.CachedResult == result,
          "completed retries must recover the original result across connections");
    auto wrongEpoch = request;
    wrongEpoch.Epoch = Identifier(2);
    wrongEpoch.Correlation = Identifier(61);
    Check(cache.Observe(wrongEpoch).Disposition == ReplayDisposition::Conflict,
          "a replay cache must never cross its host epoch boundary");
    auto abandoned = request;
    abandoned.Correlation = Identifier(62);
    Check(cache.Observe(abandoned).Disposition == ReplayDisposition::NewRequest,
          "a second correlation must enter the pending set");
    cache.Abandon(abandoned.Correlation);
    Check(cache.Observe(abandoned).Disposition == ReplayDisposition::NewRequest,
          "abandoned work must release its execute-once reservation");

    Frame readRequest{MessageKind::RequestFullSnapshot, 0, epoch, Identifier(30), 3, Identifier(63), {}};
    Check(cache.Observe(readRequest).Disposition == ReplayDisposition::NewRequest &&
              cache.Complete(readRequest.Correlation, *result) == ReplayCompletion::Stored,
          "read requests must still reserve their correlation until completion");
    const auto repeatedRead = cache.Observe(readRequest);
    Check(repeatedRead.Disposition == ReplayDisposition::NewRequest && !repeatedRead.CachedResult,
          "completed read retries must regenerate current state instead of replaying a partial response");
    cache.Abandon(readRequest.Correlation);
    for (std::uint16_t index = 0; index < 512; ++index) {
        auto read = readRequest;
        read.Correlation = Identifier(90);
        Check(cache.Observe(read).Disposition == ReplayDisposition::NewRequest &&
                  cache.Complete(read.Correlation, *result) == ReplayCompletion::Stored,
              "completed reads must release replay capacity immediately");
    }
    Check(cache.Observe(request).Disposition == ReplayDisposition::DuplicateCompleted,
          "read traffic must never evict an execute-once mutation result");
    Check(cache.Complete(Identifier(64), *result) == ReplayCompletion::NotTracked,
          "unsolicited command results must not acquire a replay entry");

    RequestReplayCache evictionCache(epoch, {4, 1, 1024, 10});
    auto firstMutation = request;
    firstMutation.Correlation = Identifier(65);
    auto secondMutation = request;
    secondMutation.Correlation = Identifier(66);
    Check(evictionCache.Observe(firstMutation).Disposition == ReplayDisposition::NewRequest &&
              evictionCache.Complete(firstMutation.Correlation, *result) == ReplayCompletion::Stored &&
              evictionCache.Observe(secondMutation).Disposition == ReplayDisposition::NewRequest &&
              evictionCache.Complete(secondMutation.Correlation, *result) == ReplayCompletion::Stored,
          "bounded replay-result storage must accept execute-once mutations");
    Check(evictionCache.Observe(firstMutation).Disposition == ReplayDisposition::DuplicateResultUnavailable,
          "evicting a result must preserve its execute-once tombstone");

    g_replayTick.store(100, std::memory_order_relaxed);
    RequestReplayCache expiryCache(epoch, {2, 1, 1024, 10}, ReplayTick);
    auto pending = request;
    pending.Correlation = Identifier(67);
    Check(expiryCache.Observe(pending).Disposition == ReplayDisposition::NewRequest,
          "a fresh request must acquire a pending reservation");
    g_replayTick.store(109, std::memory_order_relaxed);
    Check(expiryCache.Observe(pending).Disposition == ReplayDisposition::DuplicatePending,
          "a pending reservation must remain live before its absolute deadline");
    g_replayTick.store(110, std::memory_order_relaxed);
    Check(expiryCache.Observe(pending).Disposition == ReplayDisposition::PendingExpired,
          "a pending reservation must expire at its original absolute deadline");
    g_replayTick.store(1'000, std::memory_order_relaxed);
    Check(expiryCache.Observe(pending).Disposition == ReplayDisposition::PendingExpired,
          "retries must never extend or reacquire an expired execute-once reservation");

    auto capacity = request;
    capacity.Correlation = Identifier(68);
    Check(expiryCache.Observe(capacity).Disposition == ReplayDisposition::NewRequest,
          "the final ledger slot must remain available before host-epoch rotation");
    auto overCapacity = request;
    overCapacity.Correlation = Identifier(69);
    Check(expiryCache.Observe(overCapacity).Disposition == ReplayDisposition::CapacityExceeded,
          "a full replay ledger must demand host-epoch rotation instead of re-executing old correlations");

    RequestReplayCache invalidLimits(epoch, {0, 1, 1024, 10});
    Check(!invalidLimits.MatchesEpoch(epoch) &&
              invalidLimits.Observe(request).Disposition == ReplayDisposition::Conflict,
          "invalid replay limits must fail closed");
}

int RunCoreUiProtocolPeerImpl(std::wstring_view bootstrapToken) {
    using namespace apc::core_ui;
    auto bootstrap = DecodeUiLaunchBootstrap(bootstrapToken);
    auto executablePath = CurrentExecutablePath();
    if (!bootstrap || !executablePath) return 10;
    ScopedHandle coreProcess(reinterpret_cast<HANDLE>(bootstrap->InheritedCoreProcessHandle));
    DWORD inheritedFlags = 0;
    if (!coreProcess || !SetHandleInformation(coreProcess.Get(), HANDLE_FLAG_INHERIT, 0) ||
        !GetHandleInformation(coreProcess.Get(), &inheritedFlags) || (inheritedFlags & HANDLE_FLAG_INHERIT) != 0) {
        return 11;
    }
    auto coreBinding = CaptureProcessBinding(coreProcess.Get(), *executablePath);
    auto selfBinding = CaptureProcessBinding(GetCurrentProcess(), *executablePath);
    if (!coreBinding || !selfBinding || coreBinding->ProcessId != bootstrap->CoreProcessId ||
        coreBinding->CreationTime != bootstrap->CoreCreationTime) {
        return 12;
    }
    auto pipeName = PipeName(bootstrap->Launch);
    if (!pipeName || !WaitNamedPipeW(pipeName->c_str(), 5'000)) return 13;
    HANDLE rawPipe = CreateFileW(pipeName->c_str(),
                                 GENERIC_READ | FILE_WRITE_DATA,
                                 0,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                 nullptr);
    if (rawPipe == INVALID_HANDLE_VALUE) return 14;

    ServerChallenge expectedChallenge{bootstrap->Launch,
                                      {},
                                      coreBinding->ProcessId,
                                      selfBinding->ProcessId,
                                      coreBinding->CreationTime,
                                      selfBinding->CreationTime};
    SessionHandshakeContext context{expectedChallenge, bootstrap->SupportedCapabilities, std::nullopt};
    auto endpoint = SessionEndpoint::CreateBound(SessionRole::Client,
                                                 SessionEndpoint::UniquePipeHandle(rawPipe),
                                                 coreProcess.Get(),
                                                 *coreBinding,
                                                 bootstrap->Epoch,
                                                 bootstrap->Connection,
                                                 std::move(context));
    if (!endpoint) return 15;
    const auto deadline = apc::control::DeadlineAfter(10'000);
    Frame received;
    if (endpoint->Receive(received, nullptr, deadline) != IoStatus::Success) return 15;
    auto challenge = DecodeServerChallenge(received.Payload);
    auto clientNonce = GenerateSecret();
    if (!challenge || !clientNonce) return 16;
    ClientAuthenticate authentication{
        bootstrap->Launch, challenge->ServerNonce, std::move(*clientNonce), bootstrap->SupportedCapabilities};
    OutboundMessage authenticationMessage{
        MessageKind::ClientAuthenticate, {}, *EncodeClientAuthenticate(authentication)};
    if (endpoint->Send(std::move(authenticationMessage), nullptr, deadline) != IoStatus::Success ||
        endpoint->Receive(received, nullptr, deadline) != IoStatus::Success ||
        received.Kind != MessageKind::ServerAccepted) {
        return 17;
    }
    OutboundMessage request{MessageKind::RequestFullSnapshot, Identifier(200), {}};
    if (endpoint->Send(std::move(request), nullptr, deadline) != IoStatus::Success ||
        endpoint->Receive(received, nullptr, deadline) != IoStatus::Success ||
        received.Kind != MessageKind::CommandResult) {
        return 18;
    }
    auto result = DecodeCommandResult(received.Payload);
    return result && result->Status == ActionStatus::Success ? 0 : 19;
}

void TestExactPipePeerBindingAndTransport() {
    using namespace apc::core_ui;
    auto launch = GenerateIdentifier();
    auto pipeName = launch ? PipeName(*launch) : std::nullopt;
    auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
    FILETIME creation{}, exit{}, kernel{}, user{};
    Check(GetProcessId(GetCurrentProcess()) != 0, "the current pseudo-handle must expose its process id");
    Check(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE,
          "the current pseudo-handle must expose its creation time");
    std::wstring queriedPath(32'768, L'\0');
    DWORD queriedPathLength = static_cast<DWORD>(queriedPath.size());
    Check(QueryFullProcessImageNameW(GetCurrentProcess(), 0, queriedPath.data(), &queriedPathLength) != FALSE,
          "the current pseudo-handle must expose its executable path");
    queriedPath.resize(queriedPathLength);
    auto binding = CaptureProcessBinding(GetCurrentProcess(), queriedPath);
    Check(pipeName.has_value(), "the current process session must produce a UI pipe name");
    Check(security.has_value(), "the current user token must produce a private pipe ACL");
    Check(binding.has_value(), "the current executable must produce a stable process-instance binding");
    if (!pipeName || !security || !binding) return;

    HANDLE server = CreateNamedPipeW(pipeName->c_str(),
                                     PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1,
                                     4096,
                                     4096,
                                     0,
                                     security->Get());
    Check(server != INVALID_HANDLE_VALUE, "the private UI pipe must be creatable");
    if (server == INVALID_HANDLE_VALUE) return;

    HANDLE connectedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(connectedEvent != nullptr, "the pipe connection must have a completion event");
    if (!connectedEvent) {
        CloseHandle(server);
        return;
    }
    OVERLAPPED connectOperation{};
    connectOperation.hEvent = connectedEvent;
    const BOOL connectedSynchronously = ConnectNamedPipe(server, &connectOperation);
    const DWORD connectError = connectedSynchronously ? ERROR_SUCCESS : GetLastError();
    ScopedOverlappedOperation connectGuard(
        server, connectOperation, !connectedSynchronously && connectError == ERROR_IO_PENDING);
    Check(connectedSynchronously || connectError == ERROR_IO_PENDING || connectError == ERROR_PIPE_CONNECTED,
          "the server must begin one cancellable connection operation");
    if (!connectedSynchronously && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED) {
        CloseHandle(connectedEvent);
        CloseHandle(server);
        return;
    }

    HANDLE client = CreateFileW(pipeName->c_str(),
                                GENERIC_READ | FILE_WRITE_DATA,
                                0,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                nullptr);
    Check(client != INVALID_HANDLE_VALUE, "the exact UI client must connect to its private pipe");
    if (client == INVALID_HANDLE_VALUE) {
        connectGuard.CancelAndDrain();
        CloseHandle(connectedEvent);
        CloseHandle(server);
        return;
    }
    if (!connectedSynchronously && connectError == ERROR_IO_PENDING) {
        const auto waitResult = WaitForSingleObject(connectedEvent, 1'000);
        Check(waitResult == WAIT_OBJECT_0, "the private UI pipe connection must complete before its deadline");
        if (waitResult != WAIT_OBJECT_0) {
            connectGuard.CancelAndDrain();
            CloseHandle(client);
            CloseHandle(connectedEvent);
            CloseHandle(server);
            return;
        }
        DWORD ignored = 0;
        const bool completed = GetOverlappedResult(server, &connectOperation, &ignored, FALSE) != FALSE;
        Check(completed, "the overlapped pipe connection must complete successfully");
        if (!completed) {
            connectGuard.CancelAndDrain();
            CloseHandle(client);
            CloseHandle(connectedEvent);
            CloseHandle(server);
            return;
        }
        connectGuard.Complete();
    }

    Check(IsExpectedTrustedPipeClient(server, GetCurrentProcess(), *binding),
          "the server must bind the pipe to the exact process instance and executable");
    Check(IsExpectedTrustedPipeServer(client, GetCurrentProcess(), *binding),
          "the client must symmetrically bind the exact server process instance");
    auto wrongProcessId = *binding;
    ++wrongProcessId.ProcessId;
    Check(!IsExpectedTrustedPipeClient(server, GetCurrentProcess(), wrongProcessId),
          "a different expected client PID must fail closed");
    auto wrongCreationTime = *binding;
    ++wrongCreationTime.CreationTime;
    Check(!IsExpectedTrustedPipeClient(server, GetCurrentProcess(), wrongCreationTime),
          "a different expected client creation time must fail closed");
    auto missingExecutableIdentity = *binding;
    missingExecutableIdentity.ExecutableIdentity.reset();
    Check(!IsExpectedTrustedPipeClient(server, GetCurrentProcess(), missingExecutableIdentity),
          "a missing executable file identity must fail closed");

    ServerChallenge challenge{
        *launch, Secret(10), binding->ProcessId, binding->ProcessId, binding->CreationTime, binding->CreationTime};
    const auto epoch = Identifier(1);
    const auto connection = Identifier(20);
    SettingMutation mutation;
    mutation.Key = SettingKey::ShowNotifications;
    mutation.ValueKind = SettingValueKind::Boolean;
    mutation.ExpectedSettingsRevision = 5;
    mutation.BooleanValue = true;
    auto mutationPayload = EncodeSettingMutation(mutation);
    if (!mutationPayload) {
        Check(false, "the endpoint mutation fixture must be encodable");
        CloseHandle(client);
        CloseHandle(connectedEvent);
        CloseHandle(server);
        return;
    }
    SessionHandshakeContext serverContext{challenge, CapabilityDevicePicker | CapabilitySettings, std::nullopt};
    auto clientChallenge = challenge;
    clientChallenge.ServerNonce.Clear();
    SessionHandshakeContext clientContext{clientChallenge, CapabilityDevicePicker | CapabilitySettings, std::nullopt};
    g_replayTick.store(100, std::memory_order_relaxed);
    auto replayCache = std::make_shared<RequestReplayCache>(epoch, ReplayCacheLimits{100, 1, 1024, 10}, ReplayTick);
    Frame expiringRequest{
        MessageKind::SetSetting, 0, epoch, connection, 1, Identifier(41), SecureByteBuffer(*mutationPayload)};
    Check(replayCache->Observe(expiringRequest).Disposition == ReplayDisposition::NewRequest,
          "the server replay fixture must reserve one request before reconnect");
    SessionEndpoint::UniquePipeHandle serverPipe(std::exchange(server, INVALID_HANDLE_VALUE));
    SessionEndpoint::UniquePipeHandle clientPipe(std::exchange(client, INVALID_HANDLE_VALUE));
    auto serverEndpoint = SessionEndpoint::CreateBound(SessionRole::Server,
                                                       std::move(serverPipe),
                                                       GetCurrentProcess(),
                                                       *binding,
                                                       epoch,
                                                       connection,
                                                       std::move(serverContext),
                                                       replayCache);
    auto clientEndpoint = SessionEndpoint::CreateBound(SessionRole::Client,
                                                       std::move(clientPipe),
                                                       GetCurrentProcess(),
                                                       *binding,
                                                       epoch,
                                                       connection,
                                                       std::move(clientContext));
    Check(serverEndpoint && clientEndpoint, "endpoints must own only an exactly bound peer pipe");
    if (!serverEndpoint || !clientEndpoint) {
        CloseHandle(connectedEvent);
        return;
    }
    const auto deadline = apc::control::DeadlineAfter(10'000);

    OutboundMessage challengeMessage{MessageKind::ServerChallenge, {}, *EncodeServerChallenge(challenge)};
    Check(serverEndpoint->Send(std::move(challengeMessage), nullptr, deadline) == IoStatus::Success,
          "the server endpoint must serialize its challenge");
    Frame received;
    Check(clientEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "the client endpoint must validate the server challenge");

    ClientAuthenticate authentication{
        *launch, challenge.ServerNonce, Secret(50), CapabilityDevicePicker | CapabilitySettings};
    OutboundMessage authMessage{MessageKind::ClientAuthenticate, {}, *EncodeClientAuthenticate(authentication)};
    Check(clientEndpoint->Send(std::move(authMessage), nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "the authenticated client frame must cross the private channel");

    OutboundMessage acceptedMessage{
        MessageKind::ServerAccepted, {}, *EncodeServerAccepted({{}, CapabilityDevicePicker | CapabilitySettings})};
    Check(serverEndpoint->Send(std::move(acceptedMessage), nullptr, deadline) == IoStatus::Success &&
              clientEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Phase() == SessionPhase::Active && clientEndpoint->Phase() == SessionPhase::Active,
          "both endpoints must enter active state only after mutual authentication");
    Check(serverEndpoint->NegotiatedCapabilities() == (CapabilityDevicePicker | CapabilitySettings) &&
              clientEndpoint->NegotiatedCapabilities() == (CapabilityDevicePicker | CapabilitySettings) &&
              !serverEndpoint->EstablishedCapability() && !clientEndpoint->EstablishedCapability(),
          "both endpoints must agree on the active capability set");

    g_replayTick.store(110, std::memory_order_relaxed);
    OutboundMessage expiredRequest{MessageKind::SetSetting, Identifier(41), SecureByteBuffer(*mutationPayload)};
    Check(clientEndpoint->Send(std::move(expiredRequest), nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::DuplicateRequestExpired,
          "the endpoint must expose an expired execute-once reservation without re-executing it");

    const auto requestCorrelation = Identifier(40);
    OutboundMessage request{MessageKind::SetSetting, requestCorrelation, SecureByteBuffer(*mutationPayload)};
    Check(clientEndpoint->Send(request, nullptr, deadline) == IoStatus::Success,
          "a valid frame must be written over the authenticated private channel");
    Check(clientEndpoint->Send(request, nullptr, deadline) == IoStatus::DuplicateRequestPending,
          "a pending client request must be coalesced without writing a duplicate frame");
    Check(serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success && received.Kind == request.Kind &&
              received.Correlation == requestCorrelation,
          "the authenticated private channel must preserve a complete frame");

    CommandResult result{ActionStatus::Success, {12, 4, 5, 6, 7}, L""};
    OutboundMessage response{MessageKind::CommandResult, requestCorrelation, *EncodeCommandResult(result)};
    Check(serverEndpoint->Send(std::move(response), nullptr, deadline) == IoStatus::Success &&
              clientEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "the server must persist a result before returning it to the client");

    Check(clientEndpoint->Send(request, nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::DuplicateRequestReplayed &&
              clientEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "a repeated correlation must replay its cached result without re-executing the request");

    const auto evictingCorrelation = Identifier(42);
    OutboundMessage evictingRequest{MessageKind::SetSetting, evictingCorrelation, SecureByteBuffer(*mutationPayload)};
    Check(clientEndpoint->Send(std::move(evictingRequest), nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "a second execute-once request must reach the server");
    OutboundMessage evictingResponse{MessageKind::CommandResult, evictingCorrelation, *EncodeCommandResult(result)};
    Check(serverEndpoint->Send(std::move(evictingResponse), nullptr, deadline) == IoStatus::Success &&
              clientEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success,
          "a second result must complete and evict only the first result payload");
    Check(clientEndpoint->Send(request, nullptr, deadline) == IoStatus::Success &&
              serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::DuplicateRequestResultUnavailable,
          "the endpoint must preserve an execute-once tombstone after bounded result eviction");

    constexpr std::size_t producerCount = 16;
    std::array<IoStatus, producerCount> producerStatuses{};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (std::size_t index = 0; index < producerCount; ++index) {
        producers.emplace_back([&, index] {
            auto correlation = Identifier(static_cast<std::uint8_t>(100 + index));
            OutboundMessage concurrentRequest{MessageKind::SetSetting, correlation, *EncodeSettingMutation(mutation)};
            producerStatuses[index] = clientEndpoint->Send(std::move(concurrentRequest), nullptr, deadline);
        });
    }
    for (auto& producer : producers)
        producer.join();
    bool allSent = true;
    for (auto status : producerStatuses)
        allSent = allSent && status == IoStatus::Success;
    Check(allSent, "concurrent producers must receive endpoint-owned contiguous sequences");
    bool allReceived = true;
    for (std::size_t index = 0; index < producerCount; ++index) {
        allReceived = allReceived && serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success;
        replayCache->Abandon(received.Correlation);
    }
    Check(allReceived, "endpoint serialization must preserve every concurrent request");

    std::atomic<IoStatus> blockedStatus = IoStatus::Success;
    std::atomic_bool receiveStarted = false;
    std::thread blockedReceiver([&] {
        receiveStarted.store(true, std::memory_order_release);
        Frame ignored;
        blockedStatus.store(clientEndpoint->Receive(ignored, nullptr, apc::control::DeadlineAfter(30'000)),
                            std::memory_order_release);
    });
    while (!receiveStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto closeStarted = std::chrono::steady_clock::now();
    const auto closingStatus =
        clientEndpoint->Send({MessageKind::UiClosing, {}, {}}, nullptr, apc::control::DeadlineAfter(30'000));
    blockedReceiver.join();
    const auto closeDuration = std::chrono::steady_clock::now() - closeStarted;
    Check(closingStatus == IoStatus::Success && blockedStatus.load(std::memory_order_acquire) != IoStatus::Success &&
              closeDuration < std::chrono::seconds(1),
          "a successful terminal send must promptly cancel a blocked receive in the opposite direction");
    Frame afterFailure;
    Check(clientEndpoint->Receive(afterFailure, nullptr, 0) == IoStatus::Closed,
          "a terminal endpoint must reject later receives without starting new I/O");
    Check(serverEndpoint->Receive(received, nullptr, deadline) == IoStatus::Success &&
              received.Kind == MessageKind::UiClosing && serverEndpoint->Phase() == SessionPhase::Closed,
          "receiving a terminal frame must close and signal the peer endpoint");
    serverEndpoint->Close();
    clientEndpoint->Close();
    CloseHandle(connectedEvent);
}

void TestCrossProcessBootstrapAndTransport() {
    using namespace apc::core_ui;
    auto executablePath = CurrentExecutablePath();
    auto launch = GenerateIdentifier();
    auto epoch = GenerateIdentifier();
    auto connection = GenerateIdentifier();
    auto serverNonce = GenerateSecret();
    auto security = apc::control::PipeSecurityAttributes::CreateCurrentUserOnly();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!executablePath || !launch || !epoch || !connection || !serverNonce || !security ||
        !GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        Check(false, "cross-process bootstrap prerequisites must be available");
        return;
    }
    auto pipeName = PipeName(*launch);
    if (!pipeName) {
        Check(false, "cross-process bootstrap must produce a pipe name");
        return;
    }
    ScopedHandle server(CreateNamedPipeW(pipeName->c_str(),
                                         PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         1,
                                         4096,
                                         4096,
                                         0,
                                         security->Get()));
    ScopedHandle connectedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!server || !connectedEvent) {
        Check(false, "cross-process pipe and connection event must be creatable");
        return;
    }
    OVERLAPPED connectOperation{};
    connectOperation.hEvent = connectedEvent.Get();
    const BOOL connectedSynchronously = ConnectNamedPipe(server.Get(), &connectOperation);
    const DWORD connectError = connectedSynchronously ? ERROR_SUCCESS : GetLastError();
    ScopedOverlappedOperation connectGuard(
        server.Get(), connectOperation, !connectedSynchronously && connectError == ERROR_IO_PENDING);
    if (!connectedSynchronously && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED) {
        Check(false, "cross-process server must begin an overlapped connection");
        return;
    }

    HANDLE rawInheritedCore = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(),
                         GetCurrentProcess(),
                         GetCurrentProcess(),
                         &rawInheritedCore,
                         PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                         TRUE,
                         0)) {
        Check(false, "the UI child must receive a least-privilege inherited Core process handle");
        return;
    }
    ScopedHandle inheritedCore(rawInheritedCore);
    constexpr auto capabilities = CapabilityDevicePicker | CapabilitySettings;
    UiLaunchBootstrap bootstrap{*launch,
                                *epoch,
                                *connection,
                                reinterpret_cast<std::uintptr_t>(inheritedCore.Get()),
                                GetCurrentProcessId(),
                                FileTimeValue(creation),
                                capabilities};
    auto token = EncodeUiLaunchBootstrap(bootstrap);
    if (!token) {
        Check(false, "the inherited-handle bootstrap must encode");
        return;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (attributeBytes == 0 || !InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        Check(false, "the child handle allow-list must initialize");
        return;
    }
    if (!UpdateProcThreadAttribute(attributes,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &rawInheritedCore,
                                   sizeof(rawInheritedCore),
                                   nullptr,
                                   nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        Check(false, "the child handle allow-list must contain the Core process handle");
        return;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION childInfo{};
    std::wstring commandLine = L"\"" + *executablePath + L"\" --core-ui-peer " + *token;
    const BOOL created = CreateProcessW(nullptr,
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr,
                                        nullptr,
                                        &startup.StartupInfo,
                                        &childInfo);
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        Check(false, "the UI peer process must launch with only its bootstrap handle inherited");
        return;
    }
    ScopedHandle childProcess(childInfo.hProcess);
    ScopedHandle childThread(childInfo.hThread);

    bool connected = connectedSynchronously || connectError == ERROR_PIPE_CONNECTED;
    if (!connected && WaitForSingleObject(connectedEvent.Get(), 5'000) == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        connected = GetOverlappedResult(server.Get(), &connectOperation, &ignored, FALSE) != FALSE;
        connectGuard.Complete();
    }
    auto childBinding = connected ? CaptureProcessBinding(childProcess.Get(), *executablePath) : std::nullopt;
    if (!connected || !childBinding) {
        TerminateProcess(childProcess.Get(), 100);
        Check(false, "the launched UI process must connect with a capturable process binding");
        return;
    }

    ServerChallenge challenge{*launch,
                              std::move(*serverNonce),
                              GetCurrentProcessId(),
                              childBinding->ProcessId,
                              FileTimeValue(creation),
                              childBinding->CreationTime};
    SessionHandshakeContext context{challenge, capabilities, std::nullopt};
    auto replayCache = std::make_shared<RequestReplayCache>(*epoch);
    auto endpoint = SessionEndpoint::CreateBound(SessionRole::Server,
                                                 SessionEndpoint::UniquePipeHandle(server.Release()),
                                                 childProcess.Get(),
                                                 *childBinding,
                                                 *epoch,
                                                 *connection,
                                                 std::move(context),
                                                 replayCache);
    if (!endpoint) {
        TerminateProcess(childProcess.Get(), 101);
        Check(false, "the server endpoint must bind the actual child process and bootstrap metadata");
        return;
    }
    const auto deadline = apc::control::DeadlineAfter(10'000);
    Frame received;
    OutboundMessage challengeMessage{MessageKind::ServerChallenge, {}, *EncodeServerChallenge(challenge)};
    bool exchanged = endpoint->Send(std::move(challengeMessage), nullptr, deadline) == IoStatus::Success &&
                     endpoint->Receive(received, nullptr, deadline) == IoStatus::Success &&
                     received.Kind == MessageKind::ClientAuthenticate;
    OutboundMessage acceptedMessage{MessageKind::ServerAccepted, {}, *EncodeServerAccepted({{}, capabilities})};
    exchanged = exchanged && endpoint->Send(std::move(acceptedMessage), nullptr, deadline) == IoStatus::Success &&
                endpoint->Receive(received, nullptr, deadline) == IoStatus::Success &&
                received.Kind == MessageKind::RequestFullSnapshot;
    if (exchanged) {
        OutboundMessage response{MessageKind::CommandResult,
                                 received.Correlation,
                                 *EncodeCommandResult({ActionStatus::Success, {3, 1, 1, 1, 1}, L""})};
        exchanged = endpoint->Send(std::move(response), nullptr, deadline) == IoStatus::Success;
    }
    const DWORD waitResult = WaitForSingleObject(childProcess.Get(), 5'000);
    DWORD exitCode = STILL_ACTIVE;
    if (waitResult == WAIT_OBJECT_0) GetExitCodeProcess(childProcess.Get(), &exitCode);
    if (waitResult != WAIT_OBJECT_0) TerminateProcess(childProcess.Get(), 102);
    endpoint->Close();
    Check(exchanged && waitResult == WAIT_OBJECT_0 && exitCode == 0,
          "the inherited-handle bootstrap and authenticated endpoint must work across real processes");
}

} // namespace

int RunCoreUiProtocolTests() {
    TestRandomIdentifiersAndPipeNames();
    TestSequenceValidation();
    TestHandshakeAndReconnectProofs();
    TestSnapshotWireContract();
    TestActionWireContracts();
    TestFrameContractAndStrictHeaderValidation();
    TestMalformedPayloadCorpus();
    TestSessionStateMachine();
    TestReconnectSessionStateMachine();
    TestRequestReplayCache();
    TestExactPipePeerBindingAndTransport();
    TestCrossProcessBootstrapAndTransport();
    return g_failures;
}

int RunCoreUiProtocolPeer(std::wstring_view bootstrapToken) {
    return RunCoreUiProtocolPeerImpl(bootstrapToken);
}
