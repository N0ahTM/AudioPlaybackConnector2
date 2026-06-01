#pragma once

#include <chrono>
#include <tuple>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Template ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

template <typename... Args> class Event {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using Handler = std::function<void(Args...)>;
    using HandlerId = std::size_t;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    HandlerId operator+=(Handler handler) {
        auto guard = m_lock.lock_exclusive();
        HandlerId id = m_nextId++;
        m_handlers.push_back({id, std::make_shared<Handler>(std::move(handler))});
        return id;
    }

    void operator-=(HandlerId id) {
        auto guard = m_lock.lock_exclusive();
        std::erase_if(m_handlers, [id](const auto& pair) { return pair.first == id; });
    }

    template <typename... CallArgs> void operator()(CallArgs&&... args) const {
        static_assert(sizeof...(CallArgs) == sizeof...(Args), "Event argument count mismatch");
        auto snapshot = std::tuple<std::decay_t<Args>...>(std::forward<CallArgs>(args)...);
        std::vector<std::shared_ptr<Handler>> copy;
        {
            auto guard = m_lock.lock_shared();
            copy.reserve(m_handlers.size());
            for (auto& h : m_handlers) {
                copy.push_back(h.second);
            }
        }
        for (auto& h : copy) {
            try {
                std::apply([&](auto const&... values) { (*h)(values...); }, snapshot);
            } catch (...) {
            }
        }
    }

    void clear() {
        auto guard = m_lock.lock_exclusive();
        m_handlers.clear();
    }

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::vector<std::pair<HandlerId, std::shared_ptr<Handler>>> m_handlers;
    HandlerId m_nextId = 1;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// String Helpers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace util {

inline std::wstring Utf8ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for MultiByteToWideChar");
    }
    const int len =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(len == 0);
    std::wstring out(len, L'\0');
    THROW_LAST_ERROR_IF(
        0 == MultiByteToWideChar(
                 CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), len));
    return out;
}

inline std::string Utf16ToUtf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    if (utf16.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("String too long for WideCharToMultiByte");
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(len == 0);
    std::string out(len, '\0');
    THROW_LAST_ERROR_IF(0 == WideCharToMultiByte(CP_UTF8,
                                                 WC_ERR_INVALID_CHARS,
                                                 utf16.data(),
                                                 static_cast<int>(utf16.size()),
                                                 out.data(),
                                                 len,
                                                 nullptr,
                                                 nullptr));
    return out;
}

inline std::filesystem::path GetModuleFsPath(HMODULE hModule) {
    std::wstring path(MAX_PATH, L'\0');
    DWORD actual = 0;
    while (true) {
        actual = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));
        THROW_LAST_ERROR_IF(actual == 0);
        if (static_cast<size_t>(actual) >= path.size())
            path.resize(path.size() * 2);
        else
            break;
    }
    path.resize(actual);
    return std::filesystem::path(path);
}

inline std::optional<std::vector<uint8_t>> LoadResourceData(HMODULE hInst, int id, const wchar_t* type) {
    auto hRes = FindResourceW(hInst, MAKEINTRESOURCEW(id), type);
    if (!hRes) return std::nullopt;
    auto size = SizeofResource(hInst, hRes);
    if (size == 0) return std::nullopt;
    auto hData = LoadResource(hInst, hRes);
    if (!hData) return std::nullopt;
    auto* ptr = static_cast<const uint8_t*>(LockResource(hData));
    if (!ptr) return std::nullopt;
    return std::vector<uint8_t>(ptr, ptr + size);
}

inline HWND GetWindowHandle(winrt::Microsoft::UI::Xaml::Window const& window) {
    HWND hwnd = nullptr;
    if (window) {
        winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
    }
    return hwnd;
}

inline void ApplyNativeMicaBackdrop(HWND hwnd) {
    if (!hwnd) return;

    // DWMWA_SYSTEMBACKDROP_TYPE is ignored on unsupported Windows builds.
    // 2 is DWMSBT_MAINWINDOW, the DWM-native Mica backdrop.
    int backdropType = 2;
    DwmSetWindowAttribute(hwnd, static_cast<DWORD>(38), &backdropType, sizeof(backdropType));
}

inline std::wstring ReplacePlaceholders(std::wstring_view templateStr, std::wstring_view replacement) {
    std::wstring result;
    size_t pos = 0;
    while (pos < templateStr.size()) {
        auto found = templateStr.find(L"{0}", pos);
        if (found == std::wstring_view::npos) {
            result.append(templateStr.substr(pos));
            break;
        }
        result.append(templateStr.substr(pos, found - pos));
        result.append(replacement);
        pos = found + 3;
    }
    return result;
}

} // namespace util

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Window Constants //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

inline constexpr int32_t c_settingsWindowPreferredWidthDip = 540;
inline constexpr int32_t c_settingsWindowPreferredHeightDip = 580;
inline constexpr int32_t c_settingsWindowMinWidthDip = 420;
inline constexpr int32_t c_settingsWindowMinHeightDip = 360;

namespace util {

struct SettingsWindowPlacement {
    POINT position{};
    SIZE size{};
    RECT workArea{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
};

inline int32_t DipToPixel(int32_t dip, UINT dpi) {
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
    return MulDiv(dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

inline UINT GetMonitorDpi(HMONITOR monitor) {
    if (!monitor) return USER_DEFAULT_SCREEN_DPI;

    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX != 0) {
        return dpiX;
    }
    return USER_DEFAULT_SCREEN_DPI;
}

inline RECT GetMonitorWorkArea(HMONITOR monitor) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        return monitorInfo.rcWork;
    }

    RECT rcWork{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    return rcWork;
}

inline HMONITOR GetSettingsWindowTargetMonitor(std::optional<RECT> anchorRect) {
    if (anchorRect) {
        return MonitorFromRect(&*anchorRect, MONITOR_DEFAULTTONEAREST);
    }

    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

inline int32_t ClampInt(int32_t value, int32_t minValue, int32_t maxValue) {
    if (maxValue < minValue) return maxValue;
    return std::clamp(value, minValue, maxValue);
}

inline SettingsWindowPlacement CalculateSettingsWindowPlacement(std::optional<RECT> anchorRect = std::nullopt) {
    auto monitor = GetSettingsWindowTargetMonitor(anchorRect);
    auto workArea = GetMonitorWorkArea(monitor);
    auto dpi = GetMonitorDpi(monitor);

    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);
    const int32_t preferredWidth = DipToPixel(c_settingsWindowPreferredWidthDip, dpi);
    const int32_t preferredHeight = DipToPixel(c_settingsWindowPreferredHeightDip, dpi);
    const int32_t designMinWidth = DipToPixel(c_settingsWindowMinWidthDip, dpi);
    const int32_t designMinHeight = DipToPixel(c_settingsWindowMinHeightDip, dpi);
    const int32_t maxWidth = std::max<int32_t>(1, MulDiv(workWidth, 90, 100));
    const int32_t maxHeight = std::max<int32_t>(1, MulDiv(workHeight, 90, 100));
    const int32_t minWidth = std::min(designMinWidth, maxWidth);
    const int32_t minHeight = std::min(designMinHeight, maxHeight);

    const int32_t width = ClampInt(preferredWidth, minWidth, maxWidth);
    const int32_t height = ClampInt(preferredHeight, minHeight, maxHeight);

    int32_t x = workArea.left + (workWidth - width) / 2;
    int32_t y = workArea.top + (workHeight - height) / 2;

    if (anchorRect) {
        x = anchorRect->right - width;
        const int32_t spaceBelow = workArea.bottom - anchorRect->bottom;
        const int32_t spaceAbove = anchorRect->top - workArea.top;

        if (spaceBelow >= height) {
            y = anchorRect->bottom;
        } else if (spaceAbove >= height) {
            y = anchorRect->top - height;
        }
    }

    x = ClampInt(x, workArea.left, workArea.right - width);
    y = ClampInt(y, workArea.top, workArea.bottom - height);

    return SettingsWindowPlacement{POINT{x, y}, SIZE{width, height}, workArea, dpi};
}

inline SIZE GetSettingsWindowMinTrackSize(HWND hwnd) {
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (hwnd) {
        dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
    }

    auto monitor =
        hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : GetSettingsWindowTargetMonitor(std::nullopt);
    auto workArea = GetMonitorWorkArea(monitor);
    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);

    return SIZE{std::min(DipToPixel(c_settingsWindowMinWidthDip, dpi), workWidth),
                std::min(DipToPixel(c_settingsWindowMinHeightDip, dpi), workHeight)};
}

} // namespace util
