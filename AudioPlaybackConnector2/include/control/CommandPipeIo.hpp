#pragma once

#include <windows.h>
#include <control/CommandProtocol.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace apc::control {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Endpoints and Deadlines ///////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline constexpr DWORD c_pipeBufferBytes = 4 * 1024;
inline constexpr std::size_t c_pipeInstanceCount = 4;

enum class IoStatus { Success, Timeout, Cancelled, Closed, InvalidData, Failed };

[[nodiscard]] std::optional<std::wstring> PipeName();
[[nodiscard]] std::wstring PipeInstanceName(std::wstring_view baseName, std::size_t index);
[[nodiscard]] std::uint64_t DeadlineAfter(DWORD timeoutMs) noexcept;
[[nodiscard]] DWORD RemainingWait(std::uint64_t deadline) noexcept;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Bounded Transfers /////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Handles use FILE_FLAG_OVERLAPPED. Cancellation drains the outstanding operation
// before stack buffers and OVERLAPPED storage are released. Deadline zero means no timeout.
[[nodiscard]] IoStatus
ReadExact(HANDLE pipe, void* buffer, std::uint32_t byteCount, HANDLE stopEvent, std::uint64_t deadline) noexcept;
[[nodiscard]] IoStatus
WriteExact(HANDLE pipe, void const* buffer, std::uint32_t byteCount, HANDLE stopEvent, std::uint64_t deadline) noexcept;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Framed Messages ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

[[nodiscard]] IoStatus ReadRequest(HANDLE pipe, Request& request, HANDLE stopEvent, std::uint64_t deadline);
[[nodiscard]] IoStatus WriteRequest(HANDLE pipe, Request const& request, HANDLE stopEvent, std::uint64_t deadline);
[[nodiscard]] IoStatus ReadResponse(HANDLE pipe, Response& response, HANDLE stopEvent, std::uint64_t deadline);
[[nodiscard]] IoStatus WriteResponse(HANDLE pipe, Response const& response, HANDLE stopEvent, std::uint64_t deadline);
[[nodiscard]] IoStatus
ReadAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, std::uint64_t deadline) noexcept;
[[nodiscard]] IoStatus
WriteAcknowledgement(HANDLE pipe, CorrelationId correlationId, HANDLE stopEvent, std::uint64_t deadline) noexcept;

} // namespace apc::control
