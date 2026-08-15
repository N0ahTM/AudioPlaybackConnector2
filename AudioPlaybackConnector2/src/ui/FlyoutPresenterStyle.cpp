#include <pch.h>
#include <ui/FlyoutPresenterStyle.hpp>
#include <util/Util.hpp>

namespace apc::ui {

void ApplyFlyoutPresenterStyle(winrt::Microsoft::UI::Xaml::DependencyObject const& content,
                               bool useSystemBackdropEffects) noexcept {
    try {
        auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(content);
        while (parent) {
            auto presenter = parent.try_as<winrt::Microsoft::UI::Xaml::Controls::FlyoutPresenter>();
            if (presenter) {
                auto resources = winrt::Microsoft::UI::Xaml::Application::Current().Resources();
                presenter.Background(useSystemBackdropEffects
                                         ? nullptr
                                         : resources.TryLookup(winrt::box_value(L"SolidBackgroundFillColorBaseBrush"))
                                               .try_as<winrt::Microsoft::UI::Xaml::Media::Brush>());
                presenter.BorderBrush(useSystemBackdropEffects
                                          ? nullptr
                                          : resources.TryLookup(winrt::box_value(L"SurfaceStrokeColorDefaultBrush"))
                                                .try_as<winrt::Microsoft::UI::Xaml::Media::Brush>());
                presenter.BorderThickness(useSystemBackdropEffects ? winrt::Microsoft::UI::Xaml::Thickness{0}
                                                                   : winrt::Microsoft::UI::Xaml::Thickness{1});
                presenter.Padding({0});
                presenter.MinWidth(0);
                presenter.MinHeight(0);
                break;
            }
            auto menuPresenter = parent.try_as<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutPresenter>();
            if (menuPresenter) {
                if (useSystemBackdropEffects) {
                    menuPresenter.SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
                } else {
                    menuPresenter.SystemBackdrop(nullptr);
                }
                auto resources = winrt::Microsoft::UI::Xaml::Application::Current().Resources();
                menuPresenter.Background(
                    useSystemBackdropEffects
                        ? nullptr
                        : resources.TryLookup(winrt::box_value(L"SolidBackgroundFillColorBaseBrush"))
                              .try_as<winrt::Microsoft::UI::Xaml::Media::Brush>());
                menuPresenter.BorderBrush(useSystemBackdropEffects
                                              ? nullptr
                                              : resources.TryLookup(winrt::box_value(L"SurfaceStrokeColorDefaultBrush"))
                                                    .try_as<winrt::Microsoft::UI::Xaml::Media::Brush>());
                menuPresenter.BorderThickness(useSystemBackdropEffects ? winrt::Microsoft::UI::Xaml::Thickness{0}
                                                                       : winrt::Microsoft::UI::Xaml::Thickness{1});
                break;
            }
            parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(parent);
        }
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[FlyoutPresenterStyle] ERROR: ApplyFlyoutPresenterStyle failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[FlyoutPresenterStyle] ERROR: ApplyFlyoutPresenterStyle failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[FlyoutPresenterStyle] ERROR: ApplyFlyoutPresenterStyle failed");
    }
}

} // namespace apc::ui
