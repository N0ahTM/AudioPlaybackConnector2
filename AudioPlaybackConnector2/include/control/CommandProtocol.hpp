#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace apc::control {

inline constexpr uint32_t c_protocolVersion = 1;
inline constexpr uint32_t c_requestMagic = 0x32514341;
inline constexpr uint32_t c_responseMagic = 0x32524341;
inline constexpr uint32_t c_maxPayloadBytes = 64 * 1024;
inline constexpr DWORD c_pipeBufferBytes = 64 * 1024;
inline constexpr std::wstring_view c_pipeNamePrefix = L"\\\\.\\pipe\\AudioPlaybackConnector2.Control.v1.";

enum class CommandType : uint32_t {
    Unknown = 0,
    List = 1,
    Status = 2,
    Connect = 3,
    Disconnect = 4,
    Reconnect = 5,
    ToggleLast = 6,
    DisconnectAll = 7,
    ReconnectAll = 8
};

enum class TargetKind : uint32_t { None = 0, Id = 1, Name = 2, Mac = 3, Last = 4, Auto = 5 };

enum CommandFlags : uint32_t { CommandFlagNone = 0, CommandFlagJson = 1 };

enum class ExitCode : uint32_t {
    Success = 0,
    InvalidRequest = 3,
    NotFound = 4,
    Ambiguous = 5,
    OperationFailed = 6,
    Unavailable = 7
};

struct RequestHeader {
    uint32_t Magic = c_requestMagic;
    uint32_t Version = c_protocolVersion;
    uint32_t Command = 0;
    uint32_t Target = 0;
    uint32_t Flags = 0;
    uint32_t PayloadBytes = 0;
};

struct ResponseHeader {
    uint32_t Magic = c_responseMagic;
    uint32_t Version = c_protocolVersion;
    uint32_t ExitCode = 0;
    uint32_t PayloadBytes = 0;
};

struct Request {
    CommandType Command = CommandType::Unknown;
    TargetKind Target = TargetKind::None;
    uint32_t Flags = CommandFlagNone;
    std::wstring Payload;
};

struct Response {
    ExitCode Code = ExitCode::Success;
    std::wstring Payload;
};

inline std::wstring PipeName() {
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        sessionId = 0;
    }
    return std::wstring(c_pipeNamePrefix) + std::to_wstring(sessionId);
}

inline bool IsPayloadByteCountValid(uint32_t byteCount) noexcept {
    return byteCount <= c_maxPayloadBytes && byteCount % sizeof(wchar_t) == 0;
}

inline std::optional<uint32_t> PayloadByteCount(std::wstring_view payload) noexcept {
    if (payload.size() > std::numeric_limits<uint32_t>::max() / sizeof(wchar_t)) return std::nullopt;
    const auto byteCount = static_cast<uint32_t>(payload.size() * sizeof(wchar_t));
    if (!IsPayloadByteCountValid(byteCount)) return std::nullopt;
    return byteCount;
}

inline bool ReadExact(HANDLE pipe, void* buffer, uint32_t byteCount) noexcept {
    auto* cursor = static_cast<uint8_t*>(buffer);
    uint32_t remaining = byteCount;
    while (remaining > 0) {
        DWORD bytesRead = 0;
        const DWORD chunk = std::min<DWORD>(remaining, std::numeric_limits<DWORD>::max());
        if (!ReadFile(pipe, cursor, chunk, &bytesRead, nullptr) || bytesRead == 0) return false;
        cursor += bytesRead;
        remaining -= bytesRead;
    }
    return true;
}

inline bool WriteExact(HANDLE pipe, const void* buffer, uint32_t byteCount) noexcept {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    uint32_t remaining = byteCount;
    while (remaining > 0) {
        DWORD bytesWritten = 0;
        const DWORD chunk = std::min<DWORD>(remaining, std::numeric_limits<DWORD>::max());
        if (!WriteFile(pipe, cursor, chunk, &bytesWritten, nullptr) || bytesWritten == 0) return false;
        cursor += bytesWritten;
        remaining -= bytesWritten;
    }
    return true;
}

inline bool ReadRequest(HANDLE pipe, Request& request) {
    RequestHeader header{};
    if (!ReadExact(pipe, &header, sizeof(header))) return false;
    if (header.Magic != c_requestMagic || header.Version != c_protocolVersion ||
        !IsPayloadByteCountValid(header.PayloadBytes))
        return false;

    std::wstring payload(header.PayloadBytes / sizeof(wchar_t), L'\0');
    if (header.PayloadBytes > 0 && !ReadExact(pipe, payload.data(), header.PayloadBytes)) return false;

    request.Command = static_cast<CommandType>(header.Command);
    request.Target = static_cast<TargetKind>(header.Target);
    request.Flags = header.Flags;
    request.Payload = std::move(payload);
    return true;
}

inline bool WriteRequest(HANDLE pipe, Request const& request) {
    auto payloadBytes = PayloadByteCount(request.Payload);
    if (!payloadBytes) return false;

    RequestHeader header{};
    header.Command = static_cast<uint32_t>(request.Command);
    header.Target = static_cast<uint32_t>(request.Target);
    header.Flags = request.Flags;
    header.PayloadBytes = *payloadBytes;

    return WriteExact(pipe, &header, sizeof(header)) &&
           (*payloadBytes == 0 || WriteExact(pipe, request.Payload.data(), *payloadBytes));
}

inline bool ReadResponse(HANDLE pipe, Response& response) {
    ResponseHeader header{};
    if (!ReadExact(pipe, &header, sizeof(header))) return false;
    if (header.Magic != c_responseMagic || header.Version != c_protocolVersion ||
        !IsPayloadByteCountValid(header.PayloadBytes))
        return false;

    std::wstring payload(header.PayloadBytes / sizeof(wchar_t), L'\0');
    if (header.PayloadBytes > 0 && !ReadExact(pipe, payload.data(), header.PayloadBytes)) return false;

    response.Code = static_cast<ExitCode>(header.ExitCode);
    response.Payload = std::move(payload);
    return true;
}

inline bool WriteResponse(HANDLE pipe, Response const& response) {
    auto payloadBytes = PayloadByteCount(response.Payload);
    if (!payloadBytes) return false;

    ResponseHeader header{};
    header.ExitCode = static_cast<uint32_t>(response.Code);
    header.PayloadBytes = *payloadBytes;

    return WriteExact(pipe, &header, sizeof(header)) &&
           (*payloadBytes == 0 || WriteExact(pipe, response.Payload.data(), *payloadBytes));
}

} // namespace apc::control
