#include <pch.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <ui/DevicePickerView/DevicePickerView.xaml.h>
#if __has_include("DevicePickerView.g.cpp")
#include <DevicePickerView.g.cpp>
#endif

#include <util/RuntimeApartment.hpp>
#include <core/StringResources.hpp>
#include <core/SettingsLimits.hpp>
#include <ui/ButtonHelpers.hpp>
#include <util/Util.hpp>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace {
constexpr double c_pickerMinWidth = 260.0;
constexpr double c_pickerMaxWidth = 520.0;
constexpr double c_globalActionsChromeWidth = 82.0;

Button
CreateIconButton(std::wstring_view glyph, winrt::hstring const& label, Media::Brush const& foreground = nullptr) {
    Button button;
    button.Width(32);
    button.Height(32);
    button.Padding({0.0, 0.0, 0.0, 0.0});
    button.VerticalAlignment(VerticalAlignment::Center);
    button.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    button.BorderThickness({0.0, 0.0, 0.0, 0.0});
    apc::ui::SetButtonLabel(button, label);
    FontIcon icon;
    icon.FontSize(14.0);
    icon.Glyph(winrt::hstring(glyph));
    if (foreground) icon.Foreground(foreground);
    button.Content(icon);
    return button;
}

double DevicePickerWidth(bool showGlobalActions, StringResources const& strings) {
    const auto measureText = [](std::wstring_view text, double fontSize = 14.0) {
        auto block = TextBlock();
        block.Text(winrt::hstring(text));
        block.FontSize(fontSize);
        block.TextWrapping(TextWrapping::NoWrap);
        block.Measure({c_pickerMaxWidth * 2.0, 48.0});
        return block.DesiredSize().Width;
    };
    // Both pages share a width based on localized controls; long names use their full tooltip.
    auto labelWidth = std::max({measureText(strings.Get("Settings_DefaultDevice")),
                                measureText(strings.Get("DeviceOptions_Startup")),
                                measureText(strings.Get("DeviceOptions_Reconnect"))});
    auto desiredWidth = std::clamp(labelWidth + 100.0, 280.0, c_pickerMaxWidth);
    if (showGlobalActions) {
        auto actionWidth =
            std::max(measureText(strings.Get("DisconnectAll"), 12.0), measureText(strings.Get("ReconnectAll"), 12.0));
        desiredWidth = std::max(desiredWidth, 2.0 * actionWidth + c_globalActionsChromeWidth);
    }
    return std::clamp(desiredWidth, c_pickerMinWidth, c_pickerMaxWidth);
}
} // namespace

namespace winrt::AudioPlaybackConnector2::implementation {
/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DevicePickerView::DevicePickerView() {
    InitializeComponent();
    auto weak = get_weak();
    RootGrid().SizeChanged([weak](auto const&, SizeChangedEventArgs const& args) {
        if (auto self = weak.get(); self && self->m_navigationAnimation) {
            Media::RectangleGeometry clip;
            clip.Rect({0, 0, args.NewSize().Width, args.NewSize().Height});
            self->RootGrid().Clip(clip);
        }
    });
    CloseButton().Click([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnCloseClicked(sender, args);
        }
    });
    DisconnectAllButton().Click([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnDisconnectAllClicked(sender, args);
        }
    });
    ReconnectAllButton().Click([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnReconnectAllClicked(sender, args);
        }
    });
    BackButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get()) self->ReturnToDeviceList();
    });
    SettingsButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && self->SaveDeviceAlias() && self->m_onShowSettings) self->m_onShowSettings();
    });
    SavedDevicesButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            self->m_savedDevicesExpanded = !self->m_savedDevicesExpanded;
            self->RenderDeviceList(RenderReason::PresentationChanged);
        }
    });
    DeviceAliasBox().MaxLength(static_cast<int32_t>(apc::limits::c_maxDeviceAliasCharacters));
    DeviceAliasBox().LostFocus([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) static_cast<void>(self->SaveDeviceAlias());
    });
    DeviceAliasBox().KeyDown([weak](auto const&, Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            args.Handled(true);
            if (auto self = weak.get()) static_cast<void>(self->SaveDeviceAlias());
        }
    });
    ClearAliasButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            auto controller = self->m_appController.lock();
            if (!controller || !controller->ClearAlias(self->m_optionsDeviceId).Succeeded()) {
                self->ShowDeviceOptionsError();
                return;
            }
            self->DeviceOptionsError().IsOpen(false);
            self->RefreshDeviceOptions(true);
            self->RenderDeviceList(RenderReason::PresentationChanged);
        }
    });
    DefaultDeviceToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            auto controller = self->m_appController.lock();
            auto options = self->DeviceOptions(self->m_optionsDeviceId);
            bool applied = controller && options &&
                           (self->DefaultDeviceToggle().IsOn()
                                ? controller->SetDefault(self->m_optionsDeviceId).Succeeded()
                                : !options->Device.IsDefaultDevice || controller->ClearDefault().Succeeded());
            if (!applied) self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
            self->RenderDeviceList(RenderReason::PresentationChanged);
        }
    });
    DeviceStartupToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            auto controller = self->m_appController.lock();
            if (!controller ||
                controller->SetDeviceConnectOnStartup(self->m_optionsDeviceId, self->DeviceStartupToggle().IsOn())
                        .Status == SettingsMutationStatus::Rejected)
                self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
        }
    });
    DeviceReconnectToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            auto controller = self->m_appController.lock();
            if (!controller || controller
                                       ->SetDeviceReconnectOnConnectionLoss(self->m_optionsDeviceId,
                                                                            self->DeviceReconnectToggle().IsOn())
                                       .Status == SettingsMutationStatus::Rejected)
                self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
        }
    });
    ForgetDeviceButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            auto controller = self->m_appController.lock();
            auto options = self->DeviceOptions(self->m_optionsDeviceId);
            if (!controller || !options || !options->CanForget ||
                !controller->ForgetDevice(self->m_optionsDeviceId).IsApplied()) {
                self->ShowDeviceOptionsError();
                return;
            }
            self->m_savedAlias = std::wstring(self->DeviceAliasBox().Text());
            self->ReturnToDeviceList();
        }
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DevicePickerView::~DevicePickerView() {
    m_refreshCancellation.request_stop();
}

void DevicePickerView::Initialize(std::weak_ptr<apc::app::AppController> controller,
                                  std::function<void()> onClose,
                                  std::function<void()> showSettings,
                                  util::LogSink log,
                                  std::shared_ptr<StringResources const> strings) {
    m_strings = std::move(strings);
    m_log = std::move(log);
    m_appController = std::move(controller);
    m_onClose = std::move(onClose);
    m_onShowSettings = std::move(showSettings);
    ApplyLanguage();
}

bool DevicePickerView::LoadDevices() {
    if (m_preparedForRelease) return false;
    RenderDeviceList();
    if (m_isLoadingDevices || m_viewState.InventoryComplete) return true;
    auto controller = m_appController.lock();
    if (!controller) return false;
    m_refreshCancellation = std::stop_source{};
    m_isLoadingDevices = true;
    SetRefreshIndicators(true, m_viewState.Items.empty());
    RefreshDevicesAsync(get_weak(), DispatcherQueue(), std::move(controller), m_refreshCancellation.get_token(), m_log);
    return true;
}

winrt::fire_and_forget
DevicePickerView::RefreshDevicesAsync(winrt::weak_ref<DevicePickerView> weak,
                                      winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                      std::shared_ptr<apc::app::AppController> controller,
                                      std::stop_token stop,
                                      util::LogSink log) {
    try {
        co_await winrt::resume_background();
        util::RuntimeApartment apartment;
        if (apartment.Ready() && !stop.stop_requested())
            static_cast<void>(controller->ListDevices({.StopToken = stop}));
    } catch (...) {
        log.UnknownException(L"[DevicePickerView] background refresh failed");
    }
    try {
        // Only this dispatcher callback touches the view. A cancelled older request cannot finish a newer one.
        static_cast<void>(dispatcher.TryEnqueue([weak, stop, log]() noexcept {
            if (stop.stop_requested()) return;
            try {
                if (auto self = weak.get(); self && !self->m_preparedForRelease) {
                    self->m_isLoadingDevices = false;
                    self->SetRefreshIndicators(false, false);
                    self->RenderDeviceList();
                }
            } catch (...) {
                log.UnknownException(L"[DevicePickerView] refresh presentation failed");
            }
        }));
    } catch (...) {
        log.UnknownException(L"[DevicePickerView] refresh dispatch failed");
    }
}

void DevicePickerView::CancelLoadDevices() {
    m_refreshCancellation.request_stop();
    m_isLoadingDevices = false;
    SetRefreshIndicators(false, false);
}

std::optional<DeviceOptionsViewModel> DevicePickerView::DeviceOptions(std::wstring_view id) const {
    auto controller = m_appController.lock();
    if (!controller) return std::nullopt;
    return BuildDeviceOptionsViewState(controller->Snapshot(), id, m_strings->Get("Privacy_RedactedDevice"));
}

void DevicePickerView::PrepareForRelease() noexcept {
    try {
        auto dispatcher = DispatcherQueue();
        if (!dispatcher || !dispatcher.HasThreadAccess()) {
            m_log.Trace(L"[DevicePickerView] ERROR: PrepareForRelease must run on the UI thread");
            return;
        }

        m_preparedForRelease = true;
        m_presentationActive = false;
        StopNavigationAnimation();
        CancelLoadDevices();
    } catch (winrt::hresult_error const& ex) {
        m_log.Exception(L"[DevicePickerView] ERROR: PrepareForRelease failed", ex);
    } catch (std::exception const& ex) {
        m_log.Exception(L"[DevicePickerView] ERROR: PrepareForRelease failed", ex);
    } catch (...) {
        m_log.UnknownException(L"[DevicePickerView] ERROR: PrepareForRelease failed");
    }

    m_onClose = nullptr;
    m_onShowSettings = nullptr;
    m_optionsDeviceId.clear();
    m_appController.reset();
    m_renderedSnapshotGeneration = 0;
    m_hasRenderedSnapshot = false;
    m_viewState = {};
}

void DevicePickerView::ApplyLanguage() {
    if (m_preparedForRelease) return;
    TitleText().Text(winrt::hstring(m_strings->Get("TrayMenu_SelectDevice")));
    auto closeText = winrt::hstring(m_strings->Get("Close"));
    auto disconnectAllText = winrt::hstring(m_strings->Get("DisconnectAll"));
    auto reconnectAllText = winrt::hstring(m_strings->Get("ReconnectAll"));
    apc::ui::SetButtonLabel(CloseButton(), closeText);
    DisconnectAllText().Text(disconnectAllText);
    ReconnectAllText().Text(reconnectAllText);
    apc::ui::SetButtonLabel(DisconnectAllButton(), disconnectAllText);
    apc::ui::SetButtonLabel(ReconnectAllButton(), reconnectAllText);
    apc::ui::SetButtonLabel(BackButton(), winrt::hstring(m_strings->Get("DeviceOptions_Back")));
    apc::ui::SetButtonLabel(SettingsButton(), winrt::hstring(m_strings->Get("Settings_Title")));
    apc::ui::SetButtonLabel(
        SavedDevicesButton(), SavedDevicesText(), winrt::hstring(m_strings->Get("DeviceOptions_Saved")));
    apc::ui::SetButtonLabel(ClearAliasButton(), winrt::hstring(m_strings->Get("DeviceOptions_ResetName")));
    apc::ui::SetButtonLabel(ForgetDeviceButton(), winrt::hstring(m_strings->Get("DeviceOptions_Forget")));
    DeviceAliasLabel().Text(winrt::hstring(m_strings->Get("DeviceOptions_Name")));
    DefaultDeviceText().Text(winrt::hstring(m_strings->Get("Settings_DefaultDevice")));
    DeviceStartupText().Text(winrt::hstring(m_strings->Get("DeviceOptions_Startup")));
    DeviceReconnectText().Text(winrt::hstring(m_strings->Get("DeviceOptions_Reconnect")));
    Automation::AutomationProperties::SetName(DeviceAliasBox(), winrt::hstring(m_strings->Get("DeviceOptions_Name")));
    Automation::AutomationProperties::SetName(DefaultDeviceToggle(),
                                              winrt::hstring(m_strings->Get("Settings_DefaultDevice")));
    Automation::AutomationProperties::SetName(DeviceStartupToggle(),
                                              winrt::hstring(m_strings->Get("DeviceOptions_Startup")));
    Automation::AutomationProperties::SetName(DeviceReconnectToggle(),
                                              winrt::hstring(m_strings->Get("DeviceOptions_Reconnect")));
    for (auto const& toggle : {DefaultDeviceToggle(), DeviceStartupToggle(), DeviceReconnectToggle()}) {
        toggle.OnContent(box_value(L""));
        toggle.OffContent(box_value(L""));
    }
    RenderDeviceList(RenderReason::PresentationChanged);
}

void DevicePickerView::ShowDeviceOptions(std::wstring const& id) {
    if (!DeviceOptions(id)) return;
    const auto previousHeight = RootGrid().ActualHeight();
    StopNavigationAnimation();
    m_optionsDeviceId = id;
    DeviceOptionsError().IsOpen(false);
    DeviceListPanel().Visibility(Visibility::Collapsed);
    DeviceOptionsPanel().Visibility(Visibility::Visible);
    BackButton().Visibility(Visibility::Visible);
    RefreshDeviceOptions(true);
    BackButton().Focus(FocusState::Programmatic);
    AnimateNavigation(previousHeight);
}

void DevicePickerView::ReturnToDeviceList() {
    if (!SaveDeviceAlias()) return;
    const auto previousHeight = RootGrid().ActualHeight();
    StopNavigationAnimation();
    auto id = std::exchange(m_optionsDeviceId, {});
    DeviceOptionsPanel().Visibility(Visibility::Collapsed);
    DeviceListPanel().Visibility(Visibility::Visible);
    BackButton().Visibility(Visibility::Collapsed);
    TitleText().Text(winrt::hstring(m_strings->Get("TrayMenu_SelectDevice")));
    apc::ui::SetTooltipText(TitleText(), winrt::hstring(m_strings->Get("TrayMenu_SelectDevice")));
    RenderDeviceList(RenderReason::PresentationChanged);
    AnimateNavigation(previousHeight);
    for (auto const& entry : DeviceList().Items()) {
        auto item = entry.try_as<ListViewItem>();
        if (!item || unbox_value_or<winrt::hstring>(item.Tag(), L"") != id) continue;
        auto row = item.Content().as<Grid>();
        row.Children().GetAt(row.Children().Size() - 1).as<Button>().Focus(FocusState::Programmatic);
        return;
    }
    DeviceList().Focus(FocusState::Programmatic);
}

void DevicePickerView::StopNavigationAnimation() noexcept {
    ++m_navigationAnimationGeneration;
    try {
        auto animation = std::exchange(m_navigationAnimation, nullptr);
        if (animation) animation.Stop();
        RootGrid().Clip(nullptr);
    } catch (...) {
    }
}

void DevicePickerView::AnimateNavigation(double previousHeight) noexcept {
    try {
        if (!m_presentationActive || previousHeight <= 0 ||
            !winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
            return;
        using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
        ContentPanel().Measure({static_cast<float>(RootGrid().Width()), 10000.0f});
        const auto targetHeight = ContentPanel().DesiredSize().Height;
        if (targetHeight <= 0) return;
        Storyboard storyboard;
        DoubleAnimation resize;
        resize.From(previousHeight);
        resize.To(targetHeight);
        resize.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(180)));
        resize.EnableDependentAnimation(true);
        resize.FillBehavior(FillBehavior::Stop);
        CubicEase easing;
        easing.EasingMode(EasingMode::EaseOut);
        resize.EasingFunction(easing);
        Storyboard::SetTarget(resize, RootGrid());
        Storyboard::SetTargetProperty(resize, L"Height");
        storyboard.Children().Append(resize);
        DoubleAnimation fade;
        fade.From(0.2);
        fade.To(1.0);
        fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(150)));
        fade.FillBehavior(FillBehavior::Stop);
        Storyboard::SetTarget(fade,
                              m_optionsDeviceId.empty() ? DeviceListPanel().as<FrameworkElement>()
                                                        : DeviceOptionsPanel().as<FrameworkElement>());
        Storyboard::SetTargetProperty(fade, L"Opacity");
        storyboard.Children().Append(fade);
        Media::RectangleGeometry clip;
        clip.Rect({0, 0, static_cast<float>(RootGrid().Width()), static_cast<float>(previousHeight)});
        RootGrid().Clip(clip);
        const auto generation = ++m_navigationAnimationGeneration;
        storyboard.Completed([weak = get_weak(), generation](auto const&, auto const&) {
            if (auto self = weak.get(); self && self->m_navigationAnimationGeneration == generation)
                self->StopNavigationAnimation();
        });
        m_navigationAnimation = storyboard;
        storyboard.Begin();
    } catch (...) {
        m_log.UnknownException(L"[DevicePickerView] Navigation animation failed");
        StopNavigationAnimation();
    }
}

void DevicePickerView::RefreshDeviceOptions(bool resetAlias) {
    if (m_optionsDeviceId.empty()) return;
    const auto options = DeviceOptions(m_optionsDeviceId);
    if (!options) {
        m_optionsDeviceId.clear();
        ReturnToDeviceList();
        return;
    }
    m_updatingDeviceOptions = true;
    auto restoreUpdates = wil::scope_exit([this] { m_updatingDeviceOptions = false; });
    auto const& device = options->Device;
    TitleText().Text(winrt::hstring(device.DisplayName));
    apc::ui::SetTooltipText(TitleText(), winrt::hstring(device.DisplayName));
    RootGrid().Width(DevicePickerWidth(m_viewState.ConnectedDeviceCount > 1, *m_strings));
    if (resetAlias || std::wstring(DeviceAliasBox().Text()) == m_savedAlias) {
        m_savedAlias = device.Alias;
        DeviceAliasBox().Text(winrt::hstring(device.Alias));
    }
    DeviceAliasBox().PlaceholderText(winrt::hstring(device.DisplayName));
    ClearAliasButton().Visibility(device.Alias.empty() ? Visibility::Collapsed : Visibility::Visible);
    DefaultDeviceToggle().IsOn(device.IsDefaultDevice);
    DeviceStartupToggle().IsOn(options->GlobalConnectOnStartup || device.ConnectOnStartup);
    DeviceStartupToggle().IsEnabled(!options->GlobalConnectOnStartup);
    DeviceReconnectToggle().IsOn(options->GlobalReconnectOnConnectionLoss || device.ReconnectOnConnectionLoss);
    DeviceReconnectToggle().IsEnabled(!options->GlobalReconnectOnConnectionLoss);
    auto startupHelp =
        winrt::hstring(options->GlobalConnectOnStartup ? m_strings->Get("DeviceOptions_GlobalPolicy") : L"");
    auto reconnectHelp =
        winrt::hstring(options->GlobalReconnectOnConnectionLoss ? m_strings->Get("DeviceOptions_GlobalPolicy") : L"");
    apc::ui::SetTooltipText(DeviceStartupText(), startupHelp);
    apc::ui::SetTooltipText(DeviceReconnectText(), reconnectHelp);
    Automation::AutomationProperties::SetHelpText(DeviceStartupToggle(), startupHelp);
    Automation::AutomationProperties::SetHelpText(DeviceReconnectToggle(), reconnectHelp);
    ForgetDeviceButton().IsEnabled(options->CanForget);
}

bool DevicePickerView::SaveDeviceAlias() {
    if (m_preparedForRelease || m_optionsDeviceId.empty() || m_updatingDeviceOptions) return true;
    auto alias = std::wstring(DeviceAliasBox().Text());
    auto first = std::ranges::find_if_not(alias, [](wchar_t value) { return std::iswspace(value) != 0; });
    auto last = std::ranges::find_if_not(alias.rbegin(), alias.rend(), [](wchar_t value) {
                    return std::iswspace(value) != 0;
                }).base();
    alias = first < last ? std::wstring(first, last) : std::wstring{};
    if (alias == m_savedAlias) {
        DeviceAliasBox().Text(winrt::hstring(alias));
        return true;
    }
    auto controller = m_appController.lock();
    if (!controller || !controller->SetAlias(m_optionsDeviceId, alias).Succeeded()) {
        ShowDeviceOptionsError();
        return false;
    }
    m_savedAlias = alias;
    DeviceAliasBox().Text(winrt::hstring(alias));
    DeviceOptionsError().IsOpen(false);
    RefreshDeviceOptions();
    return true;
}

void DevicePickerView::ShowDeviceOptionsError() {
    DeviceOptionsError().Title(winrt::hstring(m_strings->Get("Settings_ActionFailed_Title")));
    DeviceOptionsError().Message(winrt::hstring(m_strings->Get("Settings_ActionFailed_Message")));
    DeviceOptionsError().IsOpen(true);
}

bool DevicePickerView::InvalidateDeviceInventory() {
    if (m_preparedForRelease) return true;
    RenderDeviceList();
    return !m_presentationActive || LoadDevices();
}

void DevicePickerView::SetPresentationActive(bool active) noexcept {
    m_presentationActive = active;
    if (!active) StopNavigationAnimation();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Helpers ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::RenderDeviceList(RenderReason reason) {
    auto controller = m_appController.lock();
    if (!controller) return;
    auto application = controller->Snapshot();
    if (!application.IsRunning) return;
    m_viewState = BuildDevicePickerViewState(application, m_strings->Get("Privacy_RedactedDevice"));
    RefreshDeviceOptions();
    auto const& snapshot = m_viewState;
    auto const& items = snapshot.Items;
    const auto connectedCount = snapshot.ConnectedDeviceCount;
    if (reason == RenderReason::SnapshotChanged && m_hasRenderedSnapshot &&
        m_renderedSnapshotGeneration == snapshot.Generation)
        return;

    DeviceList().Items().Clear();

    const bool anyBusy = std::any_of(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
    const auto connectedItemCount =
        std::count_if(items.begin(), items.end(), [](auto const& item) { return item.IsConnected; });
    const auto busyItemCount = std::count_if(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
    m_log.Trace(L"[DevicePickerView] RenderDeviceList connectedCount={0} itemCount={1} "
                L"connectedItemCount={2} busyItemCount={3}",
                connectedCount,
                items.size(),
                connectedItemCount,
                busyItemCount);
    ApplyGlobalActionState(connectedCount > 1, !anyBusy);
    std::vector<apc::device_picker::DeviceSnapshotItem> visibleItems;
    for (auto const& device : items) {
        if (device.IsAvailable || m_savedDevicesExpanded) visibleItems.push_back(device);
    }
    if (m_optionsDeviceId.empty()) RootGrid().Width(DevicePickerWidth(connectedCount > 1, *m_strings));
    const bool hasSavedDevices = std::ranges::any_of(items, [](auto const& device) { return !device.IsAvailable; });
    SavedDevicesButton().Visibility(hasSavedDevices ? Visibility::Visible : Visibility::Collapsed);
    SavedDevicesChevron().Glyph(m_savedDevicesExpanded ? L"\xE70E" : L"\xE70D");

    if (visibleItems.empty()) {
        auto emptyMsg = TextBlock();
        emptyMsg.Text(winrt::hstring(m_strings->Get("TrayMenu_NoDevices")));
        emptyMsg.Foreground(
            apc::ui::ThemeBrushOrFallback(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Colors::Gray()));
        emptyMsg.TextWrapping(TextWrapping::Wrap);
        DeviceList().Items().Append(emptyMsg);
    } else {
        for (auto const& device : visibleItems) {
            DeviceList().Items().Append(BuildDeviceListItem(device));
        }
    }

    m_renderedSnapshotGeneration = snapshot.Generation;
    m_hasRenderedSnapshot = true;
}

ListViewItem DevicePickerView::BuildDeviceListItem(apc::device_picker::DeviceSnapshotItem const& device) {
    auto item = ListViewItem();
    item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    item.IsTabStop(false);
    item.Padding({0, 0, 0, 0});
    const bool isBusy = device.IsBusy;

    auto grid = Grid();
    grid.HorizontalAlignment(HorizontalAlignment::Stretch);
    grid.ColumnSpacing(2);
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());

    auto weak = get_weak();
    auto id = winrt::hstring(device.Id);
    auto primary = Button();
    primary.HorizontalAlignment(HorizontalAlignment::Stretch);
    primary.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    primary.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    primary.BorderThickness({0, 0, 0, 0});
    primary.Padding({8, 6, 8, 6});
    primary.MinHeight(36);
    primary.IsEnabled(device.IsAvailable && !isBusy);
    apc::ui::SetButtonLabel(
        primary,
        winrt::hstring(std::format(L"{}: {}",
                                   device.DisplayName,
                                   device.IsConnected   ? m_strings->Get("Disconnect")
                                   : device.IsAvailable ? m_strings->Get("Connect")
                                                        : m_strings->Get("DeviceOptions_Unavailable"))));
    primary.Click([weak, id](auto const&, auto const&) {
        if (auto self = weak.get()) self->OnDeviceToggle(id);
    });

    auto primaryContent = Grid();
    primaryContent.ColumnSpacing(8);
    primaryContent.ColumnDefinitions().Append(ColumnDefinition());
    primaryContent.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
    primaryContent.ColumnDefinitions().Append(ColumnDefinition());
    primaryContent.ColumnDefinitions().Append(ColumnDefinition());
    primaryContent.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());
    if (isBusy) {
        auto busyRing = ProgressRing();
        busyRing.Width(14);
        busyRing.Height(14);
        busyRing.IsActive(true);
        primaryContent.Children().Append(busyRing);
    } else {
        auto statusIcon = FontIcon();
        statusIcon.Glyph(L"\xE702");
        statusIcon.FontSize(14);
        statusIcon.Foreground(apc::ui::TryThemeBrush(device.IsConnected ? L"SystemFillColorSuccessBrush"
                                                                        : L"TextFillColorSecondaryBrush"));
        primaryContent.Children().Append(statusIcon);
        if (device.IsConnected) {
            auto update = [log = m_log](Button const& owner) {
                auto content = owner.Content().try_as<Grid>();
                if (!content || content.Children().Size() == 0) return;
                auto current = content.Children().GetAt(0).try_as<FontIcon>();
                if (!current) return;
                const bool action = owner.IsPointerOver() || owner.FocusState() != FocusState::Unfocused;
                const winrt::hstring glyph = action ? L"\xE711" : L"\xE702";
                if (current.Glyph() == glyph) return;
                current.Glyph(glyph);
                current.Foreground(
                    apc::ui::TryThemeBrush(action ? L"SystemFillColorCriticalBrush" : L"SystemFillColorSuccessBrush"));
                log.Trace(L"[DevicePickerView] Device action icon updated: disconnect={0}", action);
            };
            // Resolve the live visual from the event sender, rather than retaining weak projected peers.
            primary.RegisterPropertyChangedCallback(
                Primitives::ButtonBase::IsPointerOverProperty(),
                [update](auto const& sender, auto const&) { update(sender.template as<Button>()); });
            primary.RegisterPropertyChangedCallback(
                UIElement::FocusStateProperty(),
                [update](auto const& sender, auto const&) { update(sender.template as<Button>()); });
            primary.Loaded([update](auto const& sender, auto const&) { update(sender.template as<Button>()); });
            // Also synchronize when moving over a row recreated under a stationary pointer.
            primary.PointerMoved([update](auto const& sender, auto const&) { update(sender.template as<Button>()); });
        }
    }

    auto nameTb = TextBlock();
    nameTb.Text(winrt::hstring(device.DisplayName));
    nameTb.MinWidth(0);
    nameTb.VerticalAlignment(VerticalAlignment::Center);
    nameTb.TextTrimming(TextTrimming::CharacterEllipsis);
    nameTb.TextWrapping(TextWrapping::NoWrap);
    nameTb.MaxLines(1);
    Grid::SetColumn(nameTb, 1);
    primaryContent.Children().Append(nameTb);
    if (device.IsDefault) {
        auto star = FontIcon();
        star.Glyph(L"\xE735");
        star.FontSize(12);
        star.Foreground(apc::ui::TryThemeBrush(L"AccentTextFillColorPrimaryBrush"));
        apc::ui::SetTooltipText(star, winrt::hstring(m_strings->Get("Settings_DefaultDevice_Current")));
        Grid::SetColumn(star, 2);
        primaryContent.Children().Append(star);
    }
    primary.Content(primaryContent);
    grid.Children().Append(primary);

    if (device.IsConnected) {
        auto reconnectBtn = CreateIconButton(L"\xE72C",
                                             winrt::hstring(m_strings->Get("Reconnect")),
                                             apc::ui::TryThemeBrush(L"TextFillColorPrimaryBrush"));
        reconnectBtn.IsEnabled(!isBusy);
        reconnectBtn.Click([weak, id](auto const&, auto const&) {
            if (auto self = weak.get()) self->OnDeviceReconnectClicked(id);
        });
        Grid::SetColumn(reconnectBtn, 1);
        grid.Children().Append(reconnectBtn);
    }

    auto optionsButton = CreateIconButton(
        L"\xE712", winrt::hstring(std::format(L"{}: {}", m_strings->Get("DeviceOptions_Title"), device.DisplayName)));
    optionsButton.Click([weak, id](auto const&, auto const&) {
        if (auto self = weak.get()) self->ShowDeviceOptions(std::wstring(id));
    });
    Grid::SetColumn(optionsButton, 2);
    grid.Children().Append(optionsButton);
    item.Content(grid);
    item.Tag(box_value(winrt::hstring(device.Id)));
    return item;
}

void DevicePickerView::ApplyGlobalActionState(bool visible, bool enabled) {
    GlobalActionsPanel().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
    DisconnectAllButton().IsEnabled(visible && enabled);
    ReconnectAllButton().IsEnabled(visible && enabled);
}

void DevicePickerView::SetRefreshIndicators(bool refreshing, bool blockingRefresh) {
    HeaderRefreshIndicator().IsActive(refreshing && !blockingRefresh);
    HeaderRefreshIndicator().Visibility(refreshing && !blockingRefresh ? Visibility::Visible : Visibility::Collapsed);
    ProgressIndicator().IsActive(refreshing && blockingRefresh);
    ProgressIndicator().Visibility(refreshing && blockingRefresh ? Visibility::Visible : Visibility::Collapsed);
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handlers ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::OnCloseClicked(winrt::Windows::Foundation::IInspectable const&,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!SaveDeviceAlias()) return;
    CancelLoadDevices();
    if (m_onClose) m_onClose();
}

void DevicePickerView::OnDeviceToggle(winrt::hstring const& id) {
    auto controller = m_appController.lock();
    auto target = apc::app::DeviceSelector::ById(std::wstring_view(id));
    if (!controller || !target) return;
    static_cast<void>(controller->Toggle(*target, apc::app::AppCommandContext::Detached()));
    if (m_onClose) m_onClose();
}

void DevicePickerView::OnDeviceReconnectClicked(winrt::hstring const& id) {
    auto controller = m_appController.lock();
    auto target = apc::app::DeviceSelector::ById(std::wstring_view(id));
    if (!controller || !target) return;
    if (m_onClose) m_onClose();
    static_cast<void>(controller->Reconnect(*target, apc::app::AppCommandContext::Detached()));
}

void DevicePickerView::OnDisconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (auto controller = m_appController.lock()) {
        static_cast<void>(controller->DisconnectAll(apc::app::AppCommandContext::Detached()));
        RenderDeviceList();
    }
}

void DevicePickerView::OnReconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (auto controller = m_appController.lock()) {
        static_cast<void>(controller->ReconnectAll(apc::app::AppCommandContext::Detached()));
        RenderDeviceList();
    }
}
} // namespace winrt::AudioPlaybackConnector2::implementation
