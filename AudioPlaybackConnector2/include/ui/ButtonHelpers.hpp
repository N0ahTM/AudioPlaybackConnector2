#pragma once
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

namespace apc::ui {

struct IconButtonOptions {
    double Width = 30.0;
    double Height = 28.0;
    double IconFontSize = 14.0;
    winrt::Microsoft::UI::Xaml::Thickness Padding{0.0, 0.0, 0.0, 0.0};
    winrt::Microsoft::UI::Xaml::Media::Brush Foreground{nullptr};
    bool TransparentBackground = true;
    bool Borderless = true;
};

inline void SetTooltipText(winrt::Microsoft::UI::Xaml::DependencyObject const& element, winrt::hstring const& text) {
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(element, winrt::box_value(text));
}

inline void SetAccessibleLabel(winrt::Microsoft::UI::Xaml::DependencyObject const& element,
                               winrt::hstring const& text) {
    SetTooltipText(element, text);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(element, text);
}

inline void SetButtonLabel(winrt::Microsoft::UI::Xaml::Controls::Button const& button, winrt::hstring const& text) {
    SetAccessibleLabel(button, text);
}

inline void SetButtonLabel(winrt::Microsoft::UI::Xaml::Controls::Button const& button,
                           winrt::Microsoft::UI::Xaml::Controls::TextBlock const& label,
                           winrt::hstring const& text) {
    label.Text(text);
    SetButtonLabel(button, text);
}

inline winrt::Microsoft::UI::Xaml::Controls::FontIcon CreateFontIcon(
    std::wstring_view glyph, double fontSize, winrt::Microsoft::UI::Xaml::Media::Brush const& foreground = nullptr) {
    auto icon = winrt::Microsoft::UI::Xaml::Controls::FontIcon();
    icon.FontSize(fontSize);
    icon.Glyph(winrt::hstring(std::wstring(glyph)));
    if (foreground) {
        icon.Foreground(foreground);
    }
    return icon;
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

inline winrt::Microsoft::UI::Xaml::Controls::StackPanel CreateIconTextContent(std::wstring_view glyph,
                                                                              winrt::hstring const& text,
                                                                              double iconFontSize = 14.0,
                                                                              double spacing = 6.0) {
    auto panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel();
    panel.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    panel.Spacing(spacing);

    panel.Children().Append(CreateFontIcon(glyph, iconFontSize));

    auto label = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
    label.Text(text);
    panel.Children().Append(label);
    return panel;
}

inline winrt::Microsoft::UI::Xaml::Controls::Button
CreateIconButton(std::wstring_view glyph, winrt::hstring const& label, IconButtonOptions const& options = {}) {
    auto button = winrt::Microsoft::UI::Xaml::Controls::Button();
    button.Width(options.Width);
    button.Height(options.Height);
    button.Padding(options.Padding);
    button.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
    if (options.TransparentBackground) {
        button.Background(
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    }
    if (options.Borderless) {
        button.BorderThickness({0.0, 0.0, 0.0, 0.0});
    }
    SetButtonLabel(button, label);
    button.Content(CreateFontIcon(glyph, options.IconFontSize, options.Foreground));
    return button;
}

} // namespace apc::ui
