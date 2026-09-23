#include <control/CommandPipeIo.hpp>

#include <appmodel.h>
#include <wil/resource.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace apc::control {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Endpoints and Deadlines ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::optional<std::wstring> PipeName() {
    constexpr std::wstring_view c_pipeNamePrefix = LR"(\\.\pipe\AudioPlaybackConnector2.Control.v2.)";
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

std::wstring PipeInstanceName(std::wstring_view baseName, std::size_t index) {
    if (index == 0) return std::wstring(baseName);
    return std::wstring(baseName) + L"." + std::to_wstring(index);
}

std::uint64_t DeadlineAfter(DWORD timeoutMs) noexcept {
    return GetTickCount64() + timeoutMs;
}

DWORD RemainingWait(std::uint64_t deadline) noexcept {
    if (deadline == 0) return INFINITE;
    const auto now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<std::uint64_t>(deadline - now, MAXDWORD - 1));
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Overlapped Transfers //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {
enum class TransferDirection { Read, Write };

IoStatus TransferExact(HANDLE pipe,
                       void* buffer,
                       std::uint32_t byteCount,
                       TransferDirection direction,
                       HANDLE stopEvent,
                       std::uint64_t deadline) noexcept {
    wil::unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return IoStatus::Failed;

    auto* cursor = static_cast<std::uint8_t*>(buffer);
    std::uint32_t remaining = byteCount;
    while (remaining > 0) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return IoStatus::Cancelled;
        if (deadline != 0 && RemainingWait(deadline) == 0) return IoStatus::Timeout;

        ResetEvent(event.get());
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const DWORD chunk = std::min<DWORD>(remaining, std::numeric_limits<DWORD>::max());
        const BOOL started = direction == TransferDirection::Write
                                 ? WriteFile(pipe, cursor, chunk, &transferred, &overlapped)
                                 : ReadFile(pipe, cursor, chunk, &transferred, &overlapped);
        if (!started) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
                    return IoStatus::Closed;
                }
                return error == ERROR_OPERATION_ABORTED ? IoStatus::Cancelled : IoStatus::Failed;
            }

            HANDLE handles[]{event.get(), stopEvent};
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

} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Framed Messages ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

IoStatus
ReadExact(HANDLE pipe, void* buffer, std::uint32_t byteCount, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    return TransferExact(pipe, buffer, byteCount, TransferDirection::Read, stopEvent, deadline);
}

IoStatus WriteExact(
    HANDLE pipe, const void* buffer, std::uint32_t byteCount, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    return TransferExact(pipe, const_cast<void*>(buffer), byteCount, TransferDirection::Write, stopEvent, deadline);
}

IoStatus ReadRequest(HANDLE pipe, Request& request, HANDLE stopEvent, std::uint64_t deadline) {
    RequestHeader header{};
    auto status = ReadExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success) return status;
    if (!IsRequestHeaderValid(header)) return IoStatus::InvalidData;
    const CorrelationId correlationId{header.CorrelationHigh, header.CorrelationLow};

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

IoStatus WriteRequest(HANDLE pipe, Request const& request, HANDLE stopEvent, std::uint64_t deadline) {
    auto payloadBytes = PayloadByteCount(request.Payload);
    if (!payloadBytes || !IsRequestValid(request)) return IoStatus::InvalidData;

    RequestHeader header{};
    header.CorrelationHigh = request.CorrelationId.High;
    header.CorrelationLow = request.CorrelationId.Low;
    header.Command = static_cast<std::uint32_t>(request.Command);
    header.Target = static_cast<std::uint32_t>(request.Target);
    header.Flags = request.Flags;
    header.PayloadBytes = *payloadBytes;

    auto status = WriteExact(pipe, &header, sizeof(header), stopEvent, deadline);
    if (status != IoStatus::Success || *payloadBytes == 0) return status;
    return WriteExact(pipe, request.Payload.data(), *payloadBytes, stopEvent, deadline);
}

IoStatus ReadResponse(HANDLE pipe, Response& response, HANDLE stopEvent, std::uint64_t deadline) {
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

IoStatus WriteResponse(HANDLE pipe, Response const& response, HANDLE stopEvent, std::uint64_t deadline) {
    const auto header = MakeResponseHeader(response);
    if (!header) return IoStatus::InvalidData;

    auto status = WriteExact(pipe, &*header, sizeof(*header), stopEvent, deadline);
    if (status != IoStatus::Success || header->PayloadBytes == 0) return status;
    return WriteExact(pipe, response.Payload.data(), header->PayloadBytes, stopEvent, deadline);
}

IoStatus
WriteAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    if (correlationId.Empty()) return IoStatus::InvalidData;
    Acknowledgement acknowledgement{};
    acknowledgement.CorrelationHigh = correlationId.High;
    acknowledgement.CorrelationLow = correlationId.Low;
    return WriteExact(pipe, &acknowledgement, sizeof(acknowledgement), stopEvent, deadline);
}

IoStatus
ReadAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, std::uint64_t deadline) noexcept {
    Acknowledgement acknowledgement{};
    const auto status = ReadExact(pipe, &acknowledgement, sizeof(acknowledgement), stopEvent, deadline);
    if (status != IoStatus::Success) return status;
    if (!IsAcknowledgementValid(acknowledgement, correlationId)) return IoStatus::InvalidData;
    return IoStatus::Success;
}

} // namespace apc::control
