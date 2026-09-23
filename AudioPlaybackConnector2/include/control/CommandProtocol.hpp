#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace apc::control {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Protocol Values ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline constexpr std::uint32_t c_protocolVersion = 2;
inline constexpr std::uint32_t c_requestMagic = 0x32514341;
inline constexpr std::uint32_t c_responseMagic = 0x32524341;
inline constexpr std::uint32_t c_acknowledgementMagic = 0x32414341;
inline constexpr std::uint32_t c_maxPayloadBytes = 64 * 1024;

enum class CommandType : std::uint32_t {
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

enum class TargetKind : std::uint32_t {
    None = 0,
    Id = 1,
    Name = 2,
    Mac = 3,
    Last = 4,
    Auto = 5,
    Alias = 6,
    Default = 7
};

enum CommandFlags : std::uint32_t { CommandFlagNone = 0, CommandFlagJson = 1, CommandFlagRaw = 2 };

enum class ExitCode : std::uint32_t {
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
    std::uint64_t High = 0;
    std::uint64_t Low = 0;

    [[nodiscard]] constexpr bool Empty() const noexcept { return High == 0 && Low == 0; }
    friend constexpr bool operator==(CorrelationId const&, CorrelationId const&) noexcept = default;
};

struct CorrelationIdHash {
    [[nodiscard]] std::size_t operator()(CorrelationId const& value) const noexcept {
        const auto mixed = value.High ^ (value.Low + 0x9E3779B97F4A7C15ull + (value.High << 6) + (value.High >> 2));
        return static_cast<std::size_t>(mixed ^ (mixed >> 32));
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Wire Messages /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct RequestHeader {
    std::uint32_t Magic = c_requestMagic;
    std::uint32_t Version = c_protocolVersion;
    std::uint64_t CorrelationHigh = 0;
    std::uint64_t CorrelationLow = 0;
    std::uint32_t Command = 0;
    std::uint32_t Target = 0;
    std::uint32_t Flags = 0;
    std::uint32_t PayloadBytes = 0;
};

struct ResponseHeader {
    std::uint32_t Magic = c_responseMagic;
    std::uint32_t Version = c_protocolVersion;
    std::uint64_t CorrelationHigh = 0;
    std::uint64_t CorrelationLow = 0;
    std::uint32_t ExitCode = 0;
    std::uint32_t PayloadBytes = 0;
};

struct Acknowledgement {
    std::uint32_t Magic = c_acknowledgementMagic;
    std::uint32_t Version = c_protocolVersion;
    std::uint64_t CorrelationHigh = 0;
    std::uint64_t CorrelationLow = 0;
};

static_assert(sizeof(RequestHeader) == 40);
static_assert(sizeof(ResponseHeader) == 32);
static_assert(sizeof(Acknowledgement) == 24);

struct Request {
    CommandType Command = CommandType::Unknown;
    TargetKind Target = TargetKind::None;
    std::uint32_t Flags = CommandFlagNone;
    std::wstring Payload;
    apc::control::CorrelationId CorrelationId;
};

struct Response {
    ExitCode Code = ExitCode::Success;
    std::wstring Payload;
    apc::control::CorrelationId CorrelationId;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Validation ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline bool IsPayloadByteCountValid(std::uint32_t byteCount) noexcept {
    return byteCount <= c_maxPayloadBytes && byteCount % sizeof(wchar_t) == 0;
}

inline bool IsKnownCommand(std::uint32_t value) noexcept {
    return value >= static_cast<std::uint32_t>(CommandType::List) &&
           value <= static_cast<std::uint32_t>(CommandType::AliasList);
}

inline bool IsKnownTarget(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(TargetKind::Default);
}

inline bool IsKnownExitCode(std::uint32_t value) noexcept {
    return value == static_cast<std::uint32_t>(ExitCode::Success) ||
           (value >= static_cast<std::uint32_t>(ExitCode::InvalidRequest) &&
            value <= static_cast<std::uint32_t>(ExitCode::Indeterminate));
}

inline bool IsRequestValid(Request const& request) noexcept {
    if (request.CorrelationId.Empty() || !IsKnownCommand(static_cast<std::uint32_t>(request.Command)) ||
        !IsKnownTarget(static_cast<std::uint32_t>(request.Target)) ||
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

inline std::optional<std::uint32_t> PayloadByteCount(std::wstring_view payload) noexcept {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(wchar_t)) return std::nullopt;
    const auto byteCount = static_cast<std::uint32_t>(payload.size() * sizeof(wchar_t));
    if (!IsPayloadByteCountValid(byteCount)) return std::nullopt;
    return byteCount;
}

inline bool IsRequestHeaderValid(RequestHeader const& header) noexcept {
    const CorrelationId correlation{header.CorrelationHigh, header.CorrelationLow};
    return header.Magic == c_requestMagic && header.Version == c_protocolVersion && !correlation.Empty() &&
           IsKnownCommand(header.Command) && IsKnownTarget(header.Target) &&
           (header.Flags & ~(CommandFlagJson | CommandFlagRaw)) == 0 && IsPayloadByteCountValid(header.PayloadBytes);
}

inline std::optional<ResponseHeader> MakeResponseHeader(Response const& response) noexcept {
    const auto payloadBytes = PayloadByteCount(response.Payload);
    if (!payloadBytes || response.CorrelationId.Empty() ||
        !IsKnownExitCode(static_cast<std::uint32_t>(response.Code))) {
        return std::nullopt;
    }
    ResponseHeader header{};
    header.CorrelationHigh = response.CorrelationId.High;
    header.CorrelationLow = response.CorrelationId.Low;
    header.ExitCode = static_cast<std::uint32_t>(response.Code);
    header.PayloadBytes = *payloadBytes;
    return header;
}

inline bool IsAcknowledgementValid(Acknowledgement const& acknowledgement, CorrelationId expected) noexcept {
    return acknowledgement.Magic == c_acknowledgementMagic && acknowledgement.Version == c_protocolVersion &&
           acknowledgement.CorrelationHigh == expected.High && acknowledgement.CorrelationLow == expected.Low;
}

} // namespace apc::control
