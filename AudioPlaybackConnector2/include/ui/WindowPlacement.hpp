#pragma once

#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

inline constexpr int32_t c_settingsWindowPreferredWidthDip = 900;
inline constexpr int32_t c_settingsWindowPreferredHeightDip = 560;
inline constexpr int32_t c_settingsWindowMinWidthDip = 660;
inline constexpr int32_t c_settingsWindowMinHeightDip = 360;
inline constexpr int32_t c_settingsWindowEdgeMarginDip = 16;

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

inline SIZE GetSettingsWindowMinTrackSizeForWorkArea(RECT const& workArea, UINT dpi) {
    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);

    return SIZE{std::min(DipToPixel(c_settingsWindowMinWidthDip, dpi), workWidth),
                std::min(DipToPixel(c_settingsWindowMinHeightDip, dpi), workHeight)};
}

inline POINT CalculateBottomRightWindowPosition(RECT const& workArea, SIZE size, UINT dpi) {
    const int32_t margin = DipToPixel(c_settingsWindowEdgeMarginDip, dpi);
    return POINT{ClampInt(workArea.right - size.cx - margin, workArea.left, workArea.right - size.cx),
                 ClampInt(workArea.bottom - size.cy - margin, workArea.top, workArea.bottom - size.cy)};
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

    auto position = CalculateBottomRightWindowPosition(workArea, SIZE{width, height}, dpi);
    int32_t x = ClampInt(position.x, workArea.left, workArea.right - width);
    int32_t y = ClampInt(position.y, workArea.top, workArea.bottom - height);

    return SettingsWindowPlacement{POINT{x, y}, SIZE{width, height}, workArea, dpi};
}

inline SettingsWindowPlacement
CalculateSettingsWindowPlacementFromSize(SIZE size, UINT persistedDpi, SettingsWindowPlacement const& basePlacement) {
    auto workArea = basePlacement.workArea;
    auto dpi = basePlacement.dpi;
    auto minTrackSize = GetSettingsWindowMinTrackSizeForWorkArea(workArea, dpi);

    if (persistedDpi == 0) persistedDpi = USER_DEFAULT_SCREEN_DPI;
    if (persistedDpi != dpi) {
        size.cx = std::max<int32_t>(1, MulDiv(size.cx, static_cast<int>(dpi), static_cast<int>(persistedDpi)));
        size.cy = std::max<int32_t>(1, MulDiv(size.cy, static_cast<int>(dpi), static_cast<int>(persistedDpi)));
    }

    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);
    const int32_t maxRestoredWidth =
        std::min<int32_t>(workWidth, std::max<int32_t>(minTrackSize.cx, basePlacement.size.cx));
    const int32_t maxRestoredHeight =
        std::min<int32_t>(workHeight, std::max<int32_t>(minTrackSize.cy, basePlacement.size.cy));
    const int32_t width = ClampInt(size.cx, minTrackSize.cx, maxRestoredWidth);
    const int32_t height = ClampInt(size.cy, minTrackSize.cy, maxRestoredHeight);
    POINT position{
        ClampInt(basePlacement.position.x + basePlacement.size.cx - width, workArea.left, workArea.right - width),
        ClampInt(basePlacement.position.y + basePlacement.size.cy - height, workArea.top, workArea.bottom - height)};

    return SettingsWindowPlacement{position, SIZE{width, height}, workArea, dpi};
}

inline SettingsWindowPlacement CalculateSettingsWindowPlacementForSize(SIZE size,
                                                                       SettingsWindowPlacement const& basePlacement) {
    auto workArea = basePlacement.workArea;
    auto dpi = basePlacement.dpi == 0 ? USER_DEFAULT_SCREEN_DPI : basePlacement.dpi;
    auto minTrackSize = GetSettingsWindowMinTrackSizeForWorkArea(workArea, dpi);

    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);
    const int32_t width = ClampInt(size.cx, minTrackSize.cx, workWidth);
    const int32_t height = ClampInt(size.cy, minTrackSize.cy, workHeight);
    auto position = CalculateBottomRightWindowPosition(workArea, SIZE{width, height}, dpi);

    return SettingsWindowPlacement{position, SIZE{width, height}, workArea, dpi};
}

inline SettingsWindowPlacement
CalculateSettingsWindowPlacementFromBounds(POINT position, SIZE size, UINT persistedDpi = USER_DEFAULT_SCREEN_DPI) {
    const auto desiredRight = std::clamp<int64_t>(static_cast<int64_t>(position.x) + std::max<int32_t>(1, size.cx),
                                                  std::numeric_limits<int32_t>::min(),
                                                  std::numeric_limits<int32_t>::max());
    const auto desiredBottom = std::clamp<int64_t>(static_cast<int64_t>(position.y) + std::max<int32_t>(1, size.cy),
                                                   std::numeric_limits<int32_t>::min(),
                                                   std::numeric_limits<int32_t>::max());
    RECT desiredRect{position.x, position.y, static_cast<int32_t>(desiredRight), static_cast<int32_t>(desiredBottom)};
    auto monitor = MonitorFromRect(&desiredRect, MONITOR_DEFAULTTONEAREST);
    auto workArea = GetMonitorWorkArea(monitor);
    auto dpi = GetMonitorDpi(monitor);
    auto minTrackSize = GetSettingsWindowMinTrackSizeForWorkArea(workArea, dpi);

    if (persistedDpi == 0) persistedDpi = USER_DEFAULT_SCREEN_DPI;
    if (persistedDpi != dpi) {
        size.cx = std::max<int32_t>(1, MulDiv(size.cx, static_cast<int>(dpi), static_cast<int>(persistedDpi)));
        size.cy = std::max<int32_t>(1, MulDiv(size.cy, static_cast<int>(dpi), static_cast<int>(persistedDpi)));
    }

    const int32_t workWidth = std::max<int32_t>(1, workArea.right - workArea.left);
    const int32_t workHeight = std::max<int32_t>(1, workArea.bottom - workArea.top);
    const int32_t width = ClampInt(size.cx, minTrackSize.cx, workWidth);
    const int32_t height = ClampInt(size.cy, minTrackSize.cy, workHeight);
    const int32_t x = ClampInt(position.x, workArea.left, workArea.right - width);
    const int32_t y = ClampInt(position.y, workArea.top, workArea.bottom - height);

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
    return GetSettingsWindowMinTrackSizeForWorkArea(workArea, dpi);
}

} // namespace util
