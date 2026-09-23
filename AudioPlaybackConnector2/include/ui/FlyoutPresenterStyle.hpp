#pragma once

#include <winrt/Microsoft.UI.Xaml.h>

#include <util/Logger.hpp>

namespace apc::ui {

void ApplyFlyoutPresenterStyle(winrt::Microsoft::UI::Xaml::DependencyObject const& content,
                               bool useSystemBackdropEffects,
                               util::LogSink const& log) noexcept;

} // namespace apc::ui
