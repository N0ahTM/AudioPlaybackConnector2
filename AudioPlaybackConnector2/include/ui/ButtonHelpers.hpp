#pragma once

#include <string_view>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Shared UI Labels and Brushes //////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace apc::ui {

inline void SetTooltipText(winrt::Microsoft::UI::Xaml::DependencyObject const& element, winrt::hstring const& text) {
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(element, winrt::box_value(text));
}

inline void SetButtonLabel(winrt::Microsoft::UI::Xaml::Controls::Button const& button, winrt::hstring const& text) {
    SetTooltipText(button, text);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, text);
}

inline void SetButtonLabel(winrt::Microsoft::UI::Xaml::Controls::Button const& button,
                           winrt::Microsoft::UI::Xaml::Controls::TextBlock const& label,
                           winrt::hstring const& text) {
    label.Text(text);
    SetButtonLabel(button, text);
}

inline winrt::Microsoft::UI::Xaml::Media::Brush TryThemeBrush(std::wstring_view resourceKey) {
    auto resource = winrt::Microsoft::UI::Xaml::Application::Current().Resources().TryLookup(
        winrt::box_value(winrt::hstring(resourceKey)));
    return resource ? resource.try_as<winrt::Microsoft::UI::Xaml::Media::Brush>() : nullptr;
}

inline winrt::Microsoft::UI::Xaml::Media::Brush ThemeBrushOrFallback(std::wstring_view resourceKey,
                                                                     winrt::Windows::UI::Color fallbackColor) {
    if (auto brush = TryThemeBrush(resourceKey)) {
        return brush;
    }
    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fallbackColor);
}

} // namespace apc::ui
