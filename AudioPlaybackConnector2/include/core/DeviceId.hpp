#pragma once

#include <core/SettingsLimits.hpp>

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace apc::core {

// A Windows device identifier is an opaque, exact identifier. Validation is
// intentionally the only normalization: changing case, whitespace, or
// punctuation could select a different device on a future platform.
class DeviceId {
public:
    [[nodiscard]] static std::optional<DeviceId> TryCreate(std::wstring_view value) {
        if (value.empty() || !apc::limits::IsBoundedUtf16(value, apc::limits::c_maxDeviceIdCharacters)) {
            return std::nullopt;
        }
        return DeviceId(std::wstring(value));
    }

    [[nodiscard]] std::wstring_view View() const noexcept { return m_value; }
    // This conversion deliberately owns its result; View() is the borrowing alternative.
    // cppcheck-suppress returnByReference
    [[nodiscard]] std::wstring ToString() const { return m_value; }

    friend bool operator==(DeviceId const&, DeviceId const&) = default;

    friend std::strong_ordering operator<=>(DeviceId const& left, DeviceId const& right) noexcept {
        if (left.m_value < right.m_value) return std::strong_ordering::less;
        if (left.m_value > right.m_value) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }

private:
    explicit DeviceId(std::wstring value) noexcept : m_value(std::move(value)) {}

    std::wstring m_value;
};

struct DeviceIdHash {
    [[nodiscard]] std::size_t operator()(DeviceId const& value) const noexcept {
        return std::hash<std::wstring_view>{}(value.View());
    }
};

} // namespace apc::core
