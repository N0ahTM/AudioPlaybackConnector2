#pragma once

#include <windows.h>
#include <appmodel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace apc::control {

inline constexpr uint32_t c_protocolVersion = 2;
inline constexpr uint32_t c_requestMagic = 0x32514341;
inline constexpr uint32_t c_responseMagic = 0x32524341;
inline constexpr uint32_t c_acknowledgementMagic = 0x32414341;
inline constexpr uint32_t c_maxPayloadBytes = 64 * 1024;
inline constexpr DWORD c_pipeBufferBytes = 4 * 1024;
inline constexpr std::size_t c_pipeInstanceCount = 4;
inline constexpr std::wstring_view c_pipeNamePrefix = L"\\\\.\\pipe\\AudioPlaybackConnector2.Control.v2.";

enum class CommandType : uint32_t {
    Unknown = 0,
    List = 1,
    Status = 2,
    Connect = 3,
    Disconnect = 4,
    Reconnect = 5,
    ToggleLast = 6,
    DisconnectAll = 7,
    ReconnectAll = 8,
    Show = 9,
    Settings = 10,
    DefaultShow = 11,
    DefaultSet = 12,
    DefaultClear = 13,
    AliasSet = 14,
    AliasClear = 15,
    AliasList = 16
};

enum class TargetKind : uint32_t { None = 0, Id = 1, Name = 2, Mac = 3, Last = 4, Auto = 5, Alias = 6, Default = 7 };

enum CommandFlags : uint32_t { CommandFlagNone = 0, CommandFlagJson = 1, CommandFlagRaw = 2 };

enum class ExitCode : uint32_t {
    Success = 0,
    InvalidRequest = 3,
    NotFound = 4,
    Ambiguous = 5,
    OperationFailed = 6,
    Unavailable = 7,
    Busy = 8,
    Indeterminate = 9
};

struct CorrelationId {
    uint64_t High = 0;
    uint64_t Low = 0;

    [[nodiscard]] constexpr bool Empty() const noexcept { return High == 0 && Low == 0; }
    friend constexpr bool operator==(CorrelationId const&, CorrelationId const&) noexcept = default;
};

struct CorrelationIdHash {
    [[nodiscard]] std::size_t operator()(CorrelationId const& value) const noexcept {
        const auto mixed = value.High ^ (value.Low + 0x9E3779B97F4A7C15ull + (value.High << 6) + (value.High >> 2));
        return static_cast<std::size_t>(mixed ^ (mixed >> 32));
    }
};

struct RequestHeader {
    uint32_t Magic = c_requestMagic;
    uint32_t Version = c_protocolVersion;
    uint64_t CorrelationHigh = 0;
    uint64_t CorrelationLow = 0;
    uint32_t Command = 0;
    uint32_t Target = 0;
    uint32_t Flags = 0;
    uint32_t PayloadBytes = 0;
};

struct ResponseHeader {
    uint32_t Magic = c_responseMagic;
    uint32_t Version = c_protocolVersion;
    uint64_t CorrelationHigh = 0;
    uint64_t CorrelationLow = 0;
    uint32_t ExitCode = 0;
    uint32_t PayloadBytes = 0;
};

struct Acknowledgement {
    uint32_t Magic = c_acknowledgementMagic;
    uint32_t Version = c_protocolVersion;
    uint64_t CorrelationHigh = 0;
    uint64_t CorrelationLow = 0;
};

static_assert(sizeof(RequestHeader) == 40);
static_assert(sizeof(ResponseHeader) == 32);
static_assert(sizeof(Acknowledgement) == 24);

struct Request {
    CommandType Command = CommandType::Unknown;
    TargetKind Target = TargetKind::None;
    uint32_t Flags = CommandFlagNone;
    std::wstring Payload;
    apc::control::CorrelationId CorrelationId;
};

struct Response {
    ExitCode Code = ExitCode::Success;
    std::wstring Payload;
    apc::control::CorrelationId CorrelationId;
};

enum class IoStatus { Success, Timeout, Cancelled, Closed, InvalidData, Failed };

inline std::optional<std::wstring> PipeName() {
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return std::nullopt;
    UINT32 packageFamilyLength = 0;
    const auto packageResult = GetCurrentPackageFamilyName(&packageFamilyLength, nullptr);
    if (packageResult == APPMODEL_ERROR_NO_PACKAGE) {
        return std::wstring(c_pipeNamePrefix) + L"dev." + std::to_wstring(sessionId);
    }
    if (packageResult != ERROR_INSUFFICIENT_BUFFER || packageFamilyLength <= 1) return std::nullopt;

    std::wstring packageFamily(packageFamilyLength, L'\0');
    if (GetCurrentPackageFamilyName(&packageFamilyLength, packageFamily.data()) != ERROR_SUCCESS ||
        packageFamilyLength <= 1) {
        return std::nullopt;
    }
    packageFamily.resize(packageFamilyLength - 1);
    return std::wstring(c_pipeNamePrefix) + L"pkg." + packageFamily + L"." + std::to_wstring(sessionId);
}

inline std::wstring PipeInstanceName(std::wstring_view baseName, std::size_t index) {
    if (index == 0) return std::wstring(baseName);
    return std::wstring(baseName) + L"." + std::to_wstring(index);
}

inline bool IsPayloadByteCountValid(uint32_t byteCount) noexcept {
    return byteCount <= c_maxPayloadBytes && byteCount % sizeof(wchar_t) == 0;
}

inline bool IsKnownCommand(uint32_t value) noexcept {
    return value >= static_cast<uint32_t>(CommandType::List) && value <= static_cast<uint32_t>(CommandType::AliasList);
}

inline bool IsKnownTarget(uint32_t value) noexcept {
    return value <= static_cast<uint32_t>(TargetKind::Default);
}

inline bool IsKnownExitCode(uint32_t value) noexcept {
    return value == static_cast<uint32_t>(ExitCode::Success) ||
           (value >= static_cast<uint32_t>(ExitCode::InvalidRequest) &&
            value <= static_cast<uint32_t>(ExitCode::Indeterminate));
}

inline bool IsRequestValid(Request const& request) noexcept {
    if (request.CorrelationId.Empty() || !IsKnownCommand(static_cast<uint32_t>(request.Command)) ||
        !IsKnownTarget(static_cast<uint32_t>(request.Target)) ||
        (request.Flags & ~(CommandFlagJson | CommandFlagRaw)) != 0 ||
        request.Payload.size() > c_maxPayloadBytes / sizeof(wchar_t) || request.Payload.contains(L'\0')) {
        return false;
    }

    const auto isExplicitTarget = [&]() noexcept {
        return request.Target == TargetKind::Id || request.Target == TargetKind::Name ||
               request.Target == TargetKind::Mac || request.Target == TargetKind::Auto ||
               request.Target == TargetKind::Alias;
    };
    const auto hasExplicitTarget = [&]() noexcept { return isExplicitTarget() && !request.Payload.empty(); };

    switch (request.Command) {
        case CommandType::List:
        case CommandType::Status:
        case CommandType::DisconnectAll:
        case CommandType::ReconnectAll:
        case CommandType::Show:
        case CommandType::Settings:
        case CommandType::DefaultShow:
        case CommandType::DefaultClear:
        case CommandType::AliasList: return request.Target == TargetKind::None && request.Payload.empty();
        case CommandType::Connect:
        case CommandType::Disconnect:
        case CommandType::Reconnect:
        case CommandType::ToggleLast:
            return hasExplicitTarget() ||
                   ((request.Target == TargetKind::Last || request.Target == TargetKind::Default) &&
                    request.Payload.empty());
        case CommandType::DefaultSet:
        case CommandType::AliasClear: return hasExplicitTarget();
        case CommandType::AliasSet: {
            if (!isExplicitTarget()) return false;
            const auto separator = request.Payload.find(L'\n');
            return separator != std::wstring::npos && separator > 0 && separator + 1 < request.Payload.size() &&
                   request.Payload.find_first_of(L"\r\n", separator + 1) == std::wstring::npos;
        }
        default: return false;
    }
}

inline std::optional<uint32_t> PayloadByteCount(std::wstring_view payload) noexcept {
    if (payload.size() > std::numeric_limits<uint32_t>::max() / sizeof(wchar_t)) return std::nullopt;
    const auto byteCount = static_cast<uint32_t>(payload.size() * sizeof(wchar_t));
    if (!IsPayloadByteCountValid(byteCount)) return std::nullopt;
    return byteCount;
}

inline uint64_t DeadlineAfter(DWORD timeoutMs) noexcept {
    return GetTickCount64() + timeoutMs;
}

inline DWORD RemainingWait(uint64_t deadline) noexcept {
    if (deadline == 0) return INFINITE;
    const auto now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<uint64_t>(deadline - now, MAXDWORD - 1));
}

inline IoStatus
TransferExact(HANDLE pipe, void* buffer, uint32_t byteCount, bool write, HANDLE stopEvent, uint64_t deadline) noexcept {
    struct EventHandle {
        HANDLE Value = nullptr;
        ~EventHandle() {
            if (Value) CloseHandle(Value);
        }
    } event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.Value) return IoStatus::Failed;

    auto* cursor = static_cast<uint8_t*>(buffer);
    uint32_t remaining = byteCount;
    while (remaining > 0) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return IoStatus::Cancelled;
        if (deadline != 0 && RemainingWait(deadline) == 0) return IoStatus::Timeout;

        ResetEvent(event.Value);
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.Value;
        DWORD transferred = 0;
        const DWORD chunk = std::min<DWORD>(remaining, std::numeric_limits<DWORD>::max());
        const BOOL started = write ? WriteFile(pipe, cursor, chunk, &transferred, &overlapped)
                                   : ReadFile(pipe, cursor, chunk, &transferred, &overlapped);
        if (!started) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
                    return IoStatus::Closed;
                }
                return error == ERROR_OPERATION_ABORTED ? IoStatus::Cancelled : IoStatus::Failed;
            }

            HANDLE handles[]{event.Value, stopEvent};
            const DWORD waitResult =
                WaitForMultipleObjects(stopEvent ? 2u : 1u, handles, FALSE, RemainingWait(deadline));
            if (waitResult == WAIT_TIMEOUT || (stopEvent && waitResult == WAIT_OBJECT_0 + 1)) {
                CancelIoEx(pipe, &overlapped);
                (void)GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                return waitResult == WAIT_TIMEOUT ? IoStatus::Timeout : IoStatus::Cancelled;
            }
            if (waitResult != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                (void)GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                return IoStatus::Failed;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                const auto completionError = GetLastError();
                if (completionError == ERROR_BROKEN_PIPE || completionError == ERROR_PIPE_NOT_CONNECTED ||
                    completionError == ERROR_NO_DATA) {
                    return IoStatus::Closed;
                }
                return completionError == ERROR_OPERATION_ABORTED ? IoStatus::Cancelled : IoStatus::Failed;
            }
        }

        if (transferred == 0) return IoStatus::Closed;
        cursor += transferred;
        remaining -= transferred;
    }
    return IoStatus::Success;
}

inline IoStatus ReadExact(HANDLE pipe, void* buffer, uint32_t byteCount, HANDLE stopEvent, uint64_t deadline) noexcept {
    return TransferExact(pipe, buffer, byteCount, false, stopEvent, deadline);
}

inline IoStatus
WriteExact(HANDLE pipe, const void* buffer, uint32_t byteCount, HANDLE stopEvent, uint64_t deadline) noexcept {
    return TransferExact(pipe, const_cast<void*>(buffer), byteCount, true, stopEvent, deadline);
}

inline IoStatus ReadRequest(HANDLE pipe, Request& request, HANDLE stopEvent, uint64_t deadline) {
    RequestHeader header{};
    auto status = ReadExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success) return status;
    const CorrelationId correlationId{header.CorrelationHigh, header.CorrelationLow};
    if (header.Magic != c_requestMagic || header.Version != c_protocolVersion || correlationId.Empty() ||
        !IsKnownCommand(header.Command) || !IsKnownTarget(header.Target) ||
        (header.Flags & ~(CommandFlagJson | CommandFlagRaw)) != 0 || !IsPayloadByteCountValid(header.PayloadBytes))
        return IoStatus::InvalidData;

    std::wstring payload(header.PayloadBytes / sizeof(wchar_t), L'\0');
    if (header.PayloadBytes > 0) {
        status = ReadExact(pipe, payload.data(), header.PayloadBytes, stopEvent, deadline);
        if (status != IoStatus::Success) return status;
    }

    request.Command = static_cast<CommandType>(header.Command);
    request.Target = static_cast<TargetKind>(header.Target);
    request.Flags = header.Flags;
    request.Payload = std::move(payload);
    request.CorrelationId = correlationId;
    return IsRequestValid(request) ? IoStatus::Success : IoStatus::InvalidData;
}

inline IoStatus WriteRequest(HANDLE pipe, Request const& request, HANDLE stopEvent, uint64_t deadline) {
    auto payloadBytes = PayloadByteCount(request.Payload);
    if (!payloadBytes || !IsRequestValid(request)) return IoStatus::InvalidData;

    RequestHeader header{};
    header.CorrelationHigh = request.CorrelationId.High;
    header.CorrelationLow = request.CorrelationId.Low;
    header.Command = static_cast<uint32_t>(request.Command);
    header.Target = static_cast<uint32_t>(request.Target);
    header.Flags = request.Flags;
    header.PayloadBytes = *payloadBytes;

    auto status = WriteExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success || *payloadBytes == 0) return status;
    return WriteExact(pipe, request.Payload.data(), *payloadBytes, stopEvent, deadline);
}

inline IoStatus ReadResponse(HANDLE pipe, Response& response, HANDLE stopEvent, uint64_t deadline) {
    ResponseHeader header{};
    auto status = ReadExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success) return status;
    const CorrelationId correlationId{header.CorrelationHigh, header.CorrelationLow};
    if (header.Magic != c_responseMagic || header.Version != c_protocolVersion || correlationId.Empty() ||
        !IsKnownExitCode(header.ExitCode) || !IsPayloadByteCountValid(header.PayloadBytes))
        return IoStatus::InvalidData;

    std::wstring payload(header.PayloadBytes / sizeof(wchar_t), L'\0');
    if (header.PayloadBytes > 0) {
        status = ReadExact(pipe, payload.data(), header.PayloadBytes, stopEvent, deadline);
        if (status != IoStatus::Success) return status;
    }

    response.Code = static_cast<ExitCode>(header.ExitCode);
    response.Payload = std::move(payload);
    response.CorrelationId = correlationId;
    return IoStatus::Success;
}

inline IoStatus WriteResponse(HANDLE pipe, Response const& response, HANDLE stopEvent, uint64_t deadline) {
    auto payloadBytes = PayloadByteCount(response.Payload);
    if (!payloadBytes || response.CorrelationId.Empty() || !IsKnownExitCode(static_cast<uint32_t>(response.Code))) {
        return IoStatus::InvalidData;
    }

    ResponseHeader header{};
    header.CorrelationHigh = response.CorrelationId.High;
    header.CorrelationLow = response.CorrelationId.Low;
    header.ExitCode = static_cast<uint32_t>(response.Code);
    header.PayloadBytes = *payloadBytes;

    auto status = WriteExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success || *payloadBytes == 0) return status;
    return WriteExact(pipe, response.Payload.data(), *payloadBytes, stopEvent, deadline);
}

inline IoStatus
WriteAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, uint64_t deadline) noexcept {
    if (correlationId.Empty()) return IoStatus::InvalidData;
    Acknowledgement acknowledgement{};
    acknowledgement.CorrelationHigh = correlationId.High;
    acknowledgement.CorrelationLow = correlationId.Low;
    return WriteExact(pipe, &acknowledgement, sizeof(acknowledgement), stopEvent, deadline);
}

inline IoStatus
ReadAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, uint64_t deadline) noexcept {
    Acknowledgement acknowledgement{};
    const auto status = ReadExact(pipe, &acknowledgement, sizeof(acknowledgement), stopEvent, deadline);
    if (status != IoStatus::Success) return status;
    if (acknowledgement.Magic != c_acknowledgementMagic || acknowledgement.Version != c_protocolVersion ||
        acknowledgement.CorrelationHigh != correlationId.High || acknowledgement.CorrelationLow != correlationId.Low) {
        return IoStatus::InvalidData;
    }
    return IoStatus::Success;
}

} // namespace apc::control
