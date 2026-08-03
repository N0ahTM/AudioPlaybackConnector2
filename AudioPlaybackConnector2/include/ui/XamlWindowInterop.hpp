#pragma once

#include <windows.h>

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/base.h>

namespace util {

inline HWND GetWindowHandle(winrt::Microsoft::UI::Xaml::Window const& window) {
    HWND hwnd = nullptr;
    if (window) {
        winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
    }
    return hwnd;
}

} // namespace util
