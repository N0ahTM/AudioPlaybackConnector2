#include <pch.h>
#include <ui/DevicePickerView/DevicePickerView.xaml.h>
#if __has_include("DevicePickerView.g.cpp")
#include <DevicePickerView.g.cpp>
#endif

#include <core/DeviceService.hpp>
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
constexpr auto c_pendingActionFallbackTimeout = std::chrono::seconds(2);
constexpr double c_pickerMinWidth = 260.0;
constexpr double c_pickerMaxWidth = 520.0;
constexpr double c_globalActionsChromeWidth = 82.0;

void CancelRefreshDevicesOperation(winrt::Windows::Foundation::IAsyncOperation<
                                       winrt::Windows::Devices::Enumeration::DeviceInformationCollection> const& op,
                                   std::wstring_view context) noexcept {
    if (!op) return;

    try {
        op.Cancel();
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[DevicePickerView] ERROR: {0} failed to cancel RefreshDevicesAsync: 0x{1:08X} {2}",
                   context,
                   static_cast<uint32_t>(ex.code()),
                   ex.message());
    } catch (std::exception const& ex) {
        DebugTrace(L"[DevicePickerView] ERROR: {0} failed to cancel RefreshDevicesAsync: {1}",
                   context,
                   util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[DevicePickerView] ERROR: {0} failed to cancel RefreshDevicesAsync: unknown exception", context);
    }
}

class DevicePickerSizer final {
public:
    [[nodiscard]] static double WidthFor(bool showGlobalActions) {
        // Both pages share a width based on localized controls; long names use their full tooltip.
        double desiredWidth = OptionsWidth();
        if (showGlobalActions) {
            desiredWidth = std::max(desiredWidth, GlobalActionsWidth());
        }

        return std::clamp(desiredWidth, c_pickerMinWidth, c_pickerMaxWidth);
    }

    [[nodiscard]] static double OptionsWidth() {
        auto labelWidth = std::max({MeasureTextWidth(_("Settings_DefaultDevice")),
                                    MeasureTextWidth(_("DeviceOptions_Startup")),
                                    MeasureTextWidth(_("DeviceOptions_Reconnect"))});
        return std::clamp(labelWidth + 100.0, 280.0, c_pickerMaxWidth);
    }

private:
    [[nodiscard]] static double MeasureTextWidth(std::wstring_view text, double fontSize = 14.0) {
        auto block = TextBlock();
        block.Text(winrt::hstring(text));
        block.FontSize(fontSize);
        block.TextWrapping(TextWrapping::NoWrap);
        block.Measure({c_pickerMaxWidth * 2.0, 48.0});
        return block.DesiredSize().Width;
    }

    [[nodiscard]] static double GlobalActionsWidth() {
        auto disconnectAll = MeasureTextWidth(winrt::hstring(_("DisconnectAll")), 12.0);
        auto reconnectAll = MeasureTextWidth(winrt::hstring(_("ReconnectAll")), 12.0);
        return 2.0 * std::max(disconnectAll, reconnectAll) + c_globalActionsChromeWidth;
    }
};
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
            self->RebuildDeviceListFromCache(false, true);
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
            if (!self->m_viewModel.SetAlias(self->m_optionsDeviceId, L"")) {
                self->ShowDeviceOptionsError();
                return;
            }
            self->DeviceOptionsError().IsOpen(false);
            self->RefreshDeviceOptions(true);
            self->RebuildDeviceListFromCache(false, true);
        }
    });
    DefaultDeviceToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            if (!self->m_viewModel.SetDefault(self->m_optionsDeviceId, self->DefaultDeviceToggle().IsOn()))
                self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
            self->RebuildDeviceListFromCache(false, true);
        }
    });
    DeviceStartupToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            if (!self->m_viewModel.SetConnectOnStartup(self->m_optionsDeviceId, self->DeviceStartupToggle().IsOn()))
                self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
        }
    });
    DeviceReconnectToggle().Toggled([weak](auto const&, auto const&) {
        if (auto self = weak.get(); self && !self->m_updatingDeviceOptions) {
            if (!self->m_viewModel.SetReconnectOnConnectionLoss(self->m_optionsDeviceId,
                                                                self->DeviceReconnectToggle().IsOn()))
                self->ShowDeviceOptionsError();
            self->RefreshDeviceOptions();
        }
    });
    ForgetDeviceButton().Click([weak](auto const&, auto const&) {
        if (auto self = weak.get()) {
            if (!self->m_viewModel.ForgetDevice(self->m_optionsDeviceId)) {
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

void DevicePickerView::Initialize(std::shared_ptr<apc::device::DeviceService> service,
                                  std::shared_ptr<SettingsStore> settingsStore,
                                  std::function<void()> onClose,
                                  std::function<void(winrt::hstring)> onDeviceSelected,
                                  std::function<void(winrt::hstring)> onDeviceDisconnect,
                                  std::function<void(winrt::hstring)> onDeviceReconnect,
                                  std::function<void()> onDisconnectAll,
                                  std::function<void()> onReconnectAll) {
    m_preparedForRelease.store(false);
    m_presentationActive.store(false, std::memory_order_release);
    m_deviceService = service;
    m_settingsStore = settingsStore;
    m_viewModel.SetDeviceService(service);
    m_viewModel.SetSettingsStore(settingsStore);
    m_onClose = std::move(onClose);
    m_onDeviceSelected = std::move(onDeviceSelected);
    m_onDeviceDisconnect = std::move(onDeviceDisconnect);
    m_onDeviceReconnect = std::move(onDeviceReconnect);
    m_onDisconnectAll = std::move(onDisconnectAll);
    m_onReconnectAll = std::move(onReconnectAll);
    TitleText().Text(winrt::hstring(_("TrayMenu_SelectDevice")));
    auto closeText = winrt::hstring(_("Close"));
    apc::ui::SetButtonLabel(CloseButton(), closeText);
    auto disconnectAllText = winrt::hstring(_("DisconnectAll"));
    auto reconnectAllText = winrt::hstring(_("ReconnectAll"));
    DisconnectAllText().Text(disconnectAllText);
    ReconnectAllText().Text(reconnectAllText);
    apc::ui::SetButtonLabel(DisconnectAllButton(), disconnectAllText);
    apc::ui::SetButtonLabel(ReconnectAllButton(), reconnectAllText);
    ApplyLanguage();
}

void DevicePickerView::SetDeviceSettings(std::shared_ptr<ISettingsController> controller,
                                         apc::app::SettingsWindowCommandExecutor::ExecuteCallback execute,
                                         std::function<void()> showSettings) {
    m_viewModel.SetDeviceSettings(std::move(controller), std::move(execute));
    m_onShowSettings = std::move(showSettings);
}

bool DevicePickerView::LoadDevices() {
    if (m_preparedForRelease.load()) {
        return false;
    }

    if (m_isLoadingDevices) {
        SetRefreshIndicators(true, !m_viewModel.HasInventory());
        return true;
    }

    const bool nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromService();
    if (m_viewModel.HasInventory()) {
        RebuildDeviceListFromCache();
    }

    if (nativeInventoryComplete || m_viewModel.IsInventoryFresh()) {
        SetRefreshIndicators(false, false);
        return true;
    }

    m_isLoadingDevices.store(true);
    m_loadDevicesCancelled.store(false);
    auto requestId = ++m_loadDevicesRequestId;
    const auto inventoryGenerationAtStart = m_inventoryGeneration->Capture();
    m_activeLoadRequestId.store(requestId);

    const bool blockingRefresh = !m_viewModel.HasInventory() && DeviceList().Items().Size() == 0;
    SetRefreshIndicators(true, blockingRefresh);

    auto dispatcher = this->DispatcherQueue();
    if (!dispatcher) {
        DebugTrace(L"[DevicePickerView] ERROR: no UI dispatcher available for LoadDevices");
        SetRefreshIndicators(false, false);
        m_isLoadingDevices.store(false);
        m_activeLoadRequestId.store(0);
        return false;
    }

    auto service = m_deviceService.lock();
    if (!service) {
        DebugTrace(L"[DevicePickerView] ERROR: no DeviceService available for LoadDevices");
        OnDeviceEnumerationFailed(blockingRefresh, requestId, inventoryGenerationAtStart);
        return false;
    }

    auto weak = get_weak();

    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
        previousOp{nullptr};
    {
        std::lock_guard lock(m_refreshDevicesOpMutex);
        previousOp = std::exchange(m_refreshDevicesOp, nullptr);
    }
    CancelRefreshDevicesOperation(previousOp, L"LoadDevices");

    try {
        auto refreshOp = service->RefreshDevicesAsync();
        {
            std::lock_guard lock(m_refreshDevicesOpMutex);
            m_refreshDevicesOp = refreshOp;
        }

        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
            pendingOp = refreshOp;

        pendingOp.Completed([weak, dispatcher, blockingRefresh, requestId, inventoryGenerationAtStart](
                                auto const& sender, winrt::Windows::Foundation::AsyncStatus status) {
            if (auto self = weak.get()) {
                std::lock_guard lock(self->m_refreshDevicesOpMutex);
                if (self->m_activeLoadRequestId.load() != requestId) {
                    return;
                }
                self->m_refreshDevicesOp = nullptr;
            }

            if (status == winrt::Windows::Foundation::AsyncStatus::Canceled) {
                bool enqueued = false;
                try {
                    enqueued = dispatcher.TryEnqueue([weak, requestId]() noexcept {
                        if (auto self = weak.get(); self && self->m_loadDevicesRequestId.load() == requestId &&
                                                    self->m_activeLoadRequestId.load() == requestId) {
                            auto completeLoad = wil::scope_exit([self, requestId]() noexcept {
                                if (self->m_loadDevicesRequestId.load() == requestId &&
                                    self->m_activeLoadRequestId.load() == requestId) {
                                    self->m_isLoadingDevices.store(false);
                                    self->m_activeLoadRequestId.store(0);
                                }
                            });
                            try {
                                self->SetRefreshIndicators(false, false);
                            } catch (winrt::hresult_error const& ex) {
                                util::DebugTraceException(L"[DevicePickerView] canceled refresh UI cleanup failed", ex);
                            } catch (std::exception const& ex) {
                                util::DebugTraceException(L"[DevicePickerView] canceled refresh UI cleanup failed", ex);
                            } catch (...) {
                                util::DebugTraceUnknownException(
                                    L"[DevicePickerView] canceled refresh UI cleanup failed");
                            }
                        }
                    });
                } catch (...) {
                    util::DebugTraceUnknownException(L"[DevicePickerView] ERROR: marshaling canceled refresh threw");
                }
                if (!enqueued) {
                    if (auto self = weak.get(); self && self->m_loadDevicesRequestId.load() == requestId) {
                        self->m_isLoadingDevices.store(false);
                        self->m_activeLoadRequestId.store(0);
                    }
                }
                return;
            }
            auto enqueueFailure = [&]() noexcept {
                bool enqueued = false;
                try {
                    enqueued = dispatcher.TryEnqueue([weak, blockingRefresh, requestId, inventoryGenerationAtStart]() {
                        if (auto self = weak.get()) {
                            self->OnDeviceEnumerationFailed(blockingRefresh, requestId, inventoryGenerationAtStart);
                        }
                    });
                } catch (...) {
                    util::DebugTraceUnknownException(
                        L"[DevicePickerView] ERROR: marshaling OnDeviceEnumerationFailed threw");
                }
                if (!enqueued) {
                    DebugTrace(L"[DevicePickerView] ERROR: failed to marshal OnDeviceEnumerationFailed to UI thread");
                    if (auto self = weak.get()) {
                        if (self->m_loadDevicesRequestId.load() == requestId) {
                            ++self->m_loadDevicesRequestId;
                            self->m_isLoadingDevices.store(false);
                            self->m_activeLoadRequestId.store(0);
                        }
                    }
                }
            };
            try {
                auto devices = sender.GetResults();
                bool enqueued =
                    dispatcher.TryEnqueue([weak, devices, blockingRefresh, requestId, inventoryGenerationAtStart]() {
                        if (auto self = weak.get()) {
                            self->ApplyDeviceResults(devices, blockingRefresh, requestId, inventoryGenerationAtStart);
                        }
                    });
                if (!enqueued) {
                    DebugTrace(L"[DevicePickerView] ERROR: failed to marshal ApplyDeviceResults to UI thread");
                    if (auto self = weak.get()) {
                        if (self->m_loadDevicesRequestId.load() == requestId) {
                            ++self->m_loadDevicesRequestId;
                            self->m_isLoadingDevices.store(false);
                            self->m_activeLoadRequestId.store(0);
                        }
                    }
                }
            } catch (winrt::hresult_error const& ex) {
                DebugTrace(L"[DevicePickerView] ERROR: RefreshDevicesAsync failed: 0x{0:08X} {1}",
                           static_cast<uint32_t>(ex.code()),
                           ex.message());
                enqueueFailure();
            } catch (std::exception const& ex) {
                DebugTrace(L"[DevicePickerView] ERROR: RefreshDevicesAsync failed: {0}", util::Utf8ToUtf16(ex.what()));
                enqueueFailure();
            } catch (...) {
                DebugTrace(L"[DevicePickerView] ERROR: RefreshDevicesAsync failed: unknown exception");
                enqueueFailure();
            }
        });
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: failed to start RefreshDevicesAsync", ex);
        OnDeviceEnumerationFailed(blockingRefresh, requestId, inventoryGenerationAtStart);
        return false;
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: failed to start RefreshDevicesAsync", ex);
        OnDeviceEnumerationFailed(blockingRefresh, requestId, inventoryGenerationAtStart);
        return false;
    } catch (...) {
        util::DebugTraceUnknownException(L"[DevicePickerView] ERROR: failed to start RefreshDevicesAsync");
        OnDeviceEnumerationFailed(blockingRefresh, requestId, inventoryGenerationAtStart);
        return false;
    }
    return true;
}

void DevicePickerView::CancelLoadDevices() {
    m_loadDevicesCancelled.store(true);
    ++m_loadDevicesRequestId;
    m_activeLoadRequestId.store(0);
    m_isLoadingDevices.store(false);
    SetRefreshIndicators(false, false);
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection> op{
        nullptr};
    {
        std::lock_guard lock(m_refreshDevicesOpMutex);
        op = std::exchange(m_refreshDevicesOp, nullptr);
    }
    CancelRefreshDevicesOperation(op, L"CancelLoadDevices");
}

void DevicePickerView::PrepareForRelease() noexcept {
    try {
        auto dispatcher = DispatcherQueue();
        if (!dispatcher || !dispatcher.HasThreadAccess()) {
            DebugTrace(L"[DevicePickerView] ERROR: PrepareForRelease must run on the UI thread");
            return;
        }

        m_preparedForRelease.store(true);
        m_presentationActive.store(false, std::memory_order_release);
        StopNavigationAnimation();
        StopPendingActionTimer();
        m_inventoryGeneration->Deactivate();
        CancelLoadDevices();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: PrepareForRelease failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: PrepareForRelease failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DevicePickerView] ERROR: PrepareForRelease failed");
    }

    m_onClose = nullptr;
    m_onDeviceSelected = nullptr;
    m_onDeviceDisconnect = nullptr;
    m_onDeviceReconnect = nullptr;
    m_onDisconnectAll = nullptr;
    m_onReconnectAll = nullptr;
    m_onShowSettings = nullptr;
    m_optionsDeviceId.clear();
    m_deviceService.reset();
    m_settingsStore.reset();
    m_pendingDeviceActions.clear();
    m_pendingGlobalAction = false;
    m_renderedSnapshotGeneration = 0;
    m_hasRenderedSnapshot = false;
    m_viewModel.Clear();
}

void DevicePickerView::RefreshDeviceStates() {
    RebuildDeviceListFromCache();
}

void DevicePickerView::ApplyLanguage() {
    if (m_preparedForRelease.load()) return;
    TitleText().Text(winrt::hstring(_("TrayMenu_SelectDevice")));
    auto closeText = winrt::hstring(_("Close"));
    auto disconnectAllText = winrt::hstring(_("DisconnectAll"));
    auto reconnectAllText = winrt::hstring(_("ReconnectAll"));
    apc::ui::SetButtonLabel(CloseButton(), closeText);
    DisconnectAllText().Text(disconnectAllText);
    ReconnectAllText().Text(reconnectAllText);
    apc::ui::SetButtonLabel(DisconnectAllButton(), disconnectAllText);
    apc::ui::SetButtonLabel(ReconnectAllButton(), reconnectAllText);
    apc::ui::SetButtonLabel(BackButton(), winrt::hstring(_("DeviceOptions_Back")));
    apc::ui::SetButtonLabel(SettingsButton(), winrt::hstring(_("Settings_Title")));
    apc::ui::SetButtonLabel(SavedDevicesButton(), SavedDevicesText(), winrt::hstring(_("DeviceOptions_Saved")));
    apc::ui::SetButtonLabel(ClearAliasButton(), winrt::hstring(_("DeviceOptions_ResetName")));
    apc::ui::SetButtonLabel(ForgetDeviceButton(), winrt::hstring(_("DeviceOptions_Forget")));
    DeviceAliasLabel().Text(winrt::hstring(_("DeviceOptions_Name")));
    DefaultDeviceText().Text(winrt::hstring(_("Settings_DefaultDevice")));
    DeviceStartupText().Text(winrt::hstring(_("DeviceOptions_Startup")));
    DeviceReconnectText().Text(winrt::hstring(_("DeviceOptions_Reconnect")));
    Automation::AutomationProperties::SetName(DeviceAliasBox(), winrt::hstring(_("DeviceOptions_Name")));
    Automation::AutomationProperties::SetName(DefaultDeviceToggle(), winrt::hstring(_("Settings_DefaultDevice")));
    Automation::AutomationProperties::SetName(DeviceStartupToggle(), winrt::hstring(_("DeviceOptions_Startup")));
    Automation::AutomationProperties::SetName(DeviceReconnectToggle(), winrt::hstring(_("DeviceOptions_Reconnect")));
    for (auto const& toggle : {DefaultDeviceToggle(), DeviceStartupToggle(), DeviceReconnectToggle()}) {
        toggle.OnContent(box_value(L""));
        toggle.OffContent(box_value(L""));
    }
    RebuildDeviceListFromCache(false, true);
}

void DevicePickerView::ShowDeviceOptions(std::wstring const& id) {
    if (!m_viewModel.DeviceOptions(id)) return;
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
    TitleText().Text(winrt::hstring(_("TrayMenu_SelectDevice")));
    apc::ui::SetTooltipText(TitleText(), winrt::hstring(_("TrayMenu_SelectDevice")));
    RebuildDeviceListFromCache(false, true);
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
        if (!m_presentationActive.load() || previousHeight <= 0 ||
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
        util::DebugTraceUnknownException(L"[DevicePickerView] Navigation animation failed");
        StopNavigationAnimation();
    }
}

void DevicePickerView::RefreshDeviceOptions(bool resetAlias) {
    if (m_optionsDeviceId.empty()) return;
    const auto options = m_viewModel.DeviceOptions(m_optionsDeviceId);
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
    RootGrid().Width(DevicePickerSizer::WidthFor(m_viewModel.CachedSnapshot().ConnectedDeviceCount > 1));
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
    auto startupHelp = winrt::hstring(options->GlobalConnectOnStartup ? _("DeviceOptions_GlobalPolicy") : L"");
    auto reconnectHelp =
        winrt::hstring(options->GlobalReconnectOnConnectionLoss ? _("DeviceOptions_GlobalPolicy") : L"");
    apc::ui::SetTooltipText(DeviceStartupText(), startupHelp);
    apc::ui::SetTooltipText(DeviceReconnectText(), reconnectHelp);
    Automation::AutomationProperties::SetHelpText(DeviceStartupToggle(), startupHelp);
    Automation::AutomationProperties::SetHelpText(DeviceReconnectToggle(), reconnectHelp);
    ForgetDeviceButton().IsEnabled(options->CanForget);
}

bool DevicePickerView::SaveDeviceAlias() {
    if (m_preparedForRelease.load() || m_optionsDeviceId.empty() || m_updatingDeviceOptions) return true;
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
    if (!m_viewModel.SetAlias(m_optionsDeviceId, alias)) {
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
    DeviceOptionsError().Title(winrt::hstring(_("Settings_ActionFailed_Title")));
    DeviceOptionsError().Message(winrt::hstring(_("Settings_ActionFailed_Message")));
    DeviceOptionsError().IsOpen(true);
}

bool DevicePickerView::InvalidateDeviceInventory() {
    if (m_preparedForRelease.load()) return true;
    static_cast<void>(m_inventoryGeneration->TryInvalidate());
    m_viewModel.InvalidateInventory();
    if (m_presentationActive.load(std::memory_order_acquire)) return LoadDevices();
    return true;
}

void DevicePickerView::SetPresentationActive(bool active) noexcept {
    m_presentationActive.store(active, std::memory_order_release);
    if (active) {
        SchedulePendingActionExpiry();
    } else {
        StopNavigationAnimation();
        StopPendingActionTimer();
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Helpers ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::ApplyDeviceResults(
    winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
    bool blockingRefresh,
    uint64_t requestId,
    uint64_t inventoryGenerationAtStart) noexcept {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (m_activeLoadRequestId.load() != requestId) return;
    auto completeLoad = [this, requestId]() noexcept {
        if (m_activeLoadRequestId.load() == requestId) {
            m_isLoadingDevices.store(false);
            m_activeLoadRequestId.store(0);
        }
    };
    auto completionGuard = wil::scope_exit([completeLoad]() noexcept { completeLoad(); });
    try {
        if (m_loadDevicesCancelled) {
            SetRefreshIndicators(false, false);
            return;
        }
        SetRefreshIndicators(false, false);

        const bool inventoryChangedDuringLoad = m_inventoryGeneration->ChangedSince(inventoryGenerationAtStart);
        bool nativeInventoryComplete = false;
        if (!devices) {
            nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromService();
            if (m_viewModel.HasInventory() || (blockingRefresh && DeviceList().Items().Size() == 0)) {
                RebuildDeviceListFromCache();
            }
        } else {
            nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromService();
            if (!m_viewModel.HasInventory()) m_viewModel.SetDevices(devices);
            if (inventoryChangedDuringLoad && !nativeInventoryComplete) m_viewModel.InvalidateInventory();
            RebuildDeviceListFromCache();
        }

        const bool retryInventory = inventoryChangedDuringLoad && !nativeInventoryComplete;
        completeLoad();
        completionGuard.release();
        if (retryInventory) static_cast<void>(LoadDevices());
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: applying device results failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: applying device results failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DevicePickerView] ERROR: applying device results failed");
    }
}

void DevicePickerView::OnDeviceEnumerationFailed(bool blockingRefresh,
                                                 uint64_t requestId,
                                                 uint64_t inventoryGenerationAtStart) noexcept {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (m_activeLoadRequestId.load() != requestId) return;
    auto completeLoad = [this, requestId]() noexcept {
        if (m_activeLoadRequestId.load() == requestId) {
            m_isLoadingDevices.store(false);
            m_activeLoadRequestId.store(0);
        }
    };
    auto completionGuard = wil::scope_exit([completeLoad]() noexcept { completeLoad(); });
    try {
        if (m_loadDevicesCancelled.load()) return;
        SetRefreshIndicators(false, false);
        if (blockingRefresh && !m_viewModel.HasInventory() && DeviceList().Items().Size() == 0) {
            RebuildDeviceListFromCache();
        }
        const bool retryInventory = m_inventoryGeneration->ChangedSince(inventoryGenerationAtStart);
        completeLoad();
        completionGuard.release();
        if (retryInventory) static_cast<void>(LoadDevices());
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: applying enumeration failure state failed", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DevicePickerView] ERROR: applying enumeration failure state failed", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DevicePickerView] ERROR: applying enumeration failure state failed");
    }
}

void DevicePickerView::RebuildDeviceListFromCache(bool reconcilePendingActions, bool forceRender) {
    RefreshDeviceOptions();
    auto const& snapshot = m_viewModel.RefreshSnapshot();
    auto const& items = snapshot.Items;
    const auto connectedCount = snapshot.ConnectedDeviceCount;
    const auto pendingDeviceCount = m_pendingDeviceActions.size();
    const bool pendingGlobalAction = m_pendingGlobalAction;
    if (reconcilePendingActions) {
        ReconcilePendingActions(items);
    }
    const bool pendingStateChanged =
        pendingDeviceCount != m_pendingDeviceActions.size() || pendingGlobalAction != m_pendingGlobalAction;
    if (!forceRender && !pendingStateChanged && m_hasRenderedSnapshot &&
        m_renderedSnapshotGeneration == snapshot.Generation) {
        SchedulePendingActionExpiry();
        return;
    }

    DeviceList().Items().Clear();

    const bool anyBusy = std::any_of(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
    const auto connectedItemCount =
        std::count_if(items.begin(), items.end(), [](auto const& item) { return item.IsConnected; });
    const auto busyItemCount = std::count_if(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
    DebugTrace(L"[DevicePickerView] RebuildDeviceListFromCache connectedCount={0} itemCount={1} "
               L"connectedItemCount={2} busyItemCount={3} pendingGlobalAction={4}",
               connectedCount,
               items.size(),
               connectedItemCount,
               busyItemCount,
               m_pendingGlobalAction);
    ApplyGlobalActionState(connectedCount > 1, !anyBusy && !m_pendingGlobalAction);
    std::vector<apc::device_picker::DeviceSnapshotItem> visibleItems;
    for (auto const& device : items) {
        if (device.IsAvailable || m_savedDevicesExpanded) visibleItems.push_back(device);
    }
    if (m_optionsDeviceId.empty()) RootGrid().Width(DevicePickerSizer::WidthFor(connectedCount > 1));
    const bool hasSavedDevices = std::ranges::any_of(items, [](auto const& device) { return !device.IsAvailable; });
    SavedDevicesButton().Visibility(hasSavedDevices ? Visibility::Visible : Visibility::Collapsed);
    SavedDevicesChevron().Glyph(m_savedDevicesExpanded ? L"\xE70E" : L"\xE70D");

    if (visibleItems.empty()) {
        auto emptyMsg = TextBlock();
        emptyMsg.Text(winrt::hstring(_("TrayMenu_NoDevices")));
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
    SchedulePendingActionExpiry();
}

ListViewItem DevicePickerView::BuildDeviceListItem(apc::device_picker::DeviceSnapshotItem const& device) {
    auto item = ListViewItem();
    item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    item.IsTabStop(false);
    item.Padding({0, 0, 0, 0});
    const bool isBusy = device.IsBusy || m_pendingGlobalAction || IsDeviceActionPending(winrt::hstring(device.Id));

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
    apc::ui::SetButtonLabel(primary,
                            winrt::hstring(std::format(L"{}: {}",
                                                       device.DisplayName,
                                                       device.IsConnected   ? _("Disconnect")
                                                       : device.IsAvailable ? _("Connect")
                                                                            : _("DeviceOptions_Unavailable"))));
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
            auto update = [](Button const& owner) {
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
                DebugTrace(L"[DevicePickerView] Device action icon updated: disconnect={0}", action);
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
        apc::ui::SetTooltipText(star, winrt::hstring(_("Settings_DefaultDevice_Current")));
        Grid::SetColumn(star, 2);
        primaryContent.Children().Append(star);
    }
    primary.Content(primaryContent);
    grid.Children().Append(primary);

    if (device.IsConnected) {
        apc::ui::IconButtonOptions reconnectOptions;
        reconnectOptions.Width = 32;
        reconnectOptions.Height = 32;
        reconnectOptions.Foreground = apc::ui::TryThemeBrush(L"TextFillColorPrimaryBrush");
        auto reconnectBtn = apc::ui::CreateIconButton(L"\xE72C", winrt::hstring(_("Reconnect")), reconnectOptions);
        reconnectBtn.IsEnabled(!isBusy);
        reconnectBtn.Click([weak, id](auto const&, auto const&) {
            if (auto self = weak.get()) self->OnDeviceReconnectClicked(id);
        });
        Grid::SetColumn(reconnectBtn, 1);
        grid.Children().Append(reconnectBtn);
    }

    apc::ui::IconButtonOptions options;
    options.Width = 32;
    options.Height = 32;
    auto optionsButton = apc::ui::CreateIconButton(
        L"\xE712", winrt::hstring(std::format(L"{}: {}", _("DeviceOptions_Title"), device.DisplayName)), options);
    optionsButton.Click([weak, id](auto const&, auto const&) {
        if (auto self = weak.get()) self->ShowDeviceOptions(std::wstring(id));
    });
    Grid::SetColumn(optionsButton, 2);
    grid.Children().Append(optionsButton);
    item.Content(grid);
    item.Tag(box_value(winrt::hstring(device.Id)));
    return item;
}

bool DevicePickerView::BeginPendingDeviceAction(winrt::hstring const& id) {
    if (id.empty() || m_pendingGlobalAction || IsDeviceActionPending(id)) return false;
    m_pendingDeviceActions[std::wstring(id)] = std::chrono::steady_clock::now();
    return true;
}

bool DevicePickerView::BeginPendingGlobalAction() {
    if (m_pendingGlobalAction) return false;
    m_pendingGlobalAction = true;
    m_pendingGlobalActionStarted = std::chrono::steady_clock::now();
    RebuildDeviceListFromCache(false, true);
    return true;
}

bool DevicePickerView::IsDeviceActionPending(winrt::hstring const& id) const {
    return m_pendingDeviceActions.contains(std::wstring(id));
}

void DevicePickerView::ReconcilePendingActions(std::vector<apc::device_picker::DeviceSnapshotItem> const& items) {
    auto const now = std::chrono::steady_clock::now();
    for (auto it = m_pendingDeviceActions.begin(); it != m_pendingDeviceActions.end();) {
        auto item = std::ranges::find(items, it->first, &apc::device_picker::DeviceSnapshotItem::Id);
        const bool managerOwnsBusy = item != items.end() && item->IsBusy;
        const bool expired = now - it->second >= c_pendingActionFallbackTimeout;
        if (managerOwnsBusy || expired) {
            it = m_pendingDeviceActions.erase(it);
        } else {
            ++it;
        }
    }

    if (m_pendingGlobalAction) {
        const bool anyBusy = std::any_of(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
        const bool expired = now - m_pendingGlobalActionStarted >= c_pendingActionFallbackTimeout;
        if (anyBusy || expired) {
            m_pendingGlobalAction = false;
            m_pendingGlobalActionStarted = {};
        }
    }
}

void DevicePickerView::SchedulePendingActionExpiry() noexcept {
    if (!m_presentationActive.load(std::memory_order_acquire) ||
        (m_pendingDeviceActions.empty() && !m_pendingGlobalAction)) {
        StopPendingActionTimer();
        return;
    }

    try {
        auto earliest = std::chrono::steady_clock::time_point::max();
        for (auto const& [id, startedAt] : m_pendingDeviceActions) {
            (void)id;
            earliest = std::min(earliest, startedAt + c_pendingActionFallbackTimeout);
        }
        if (m_pendingGlobalAction) {
            earliest = std::min(earliest, m_pendingGlobalActionStarted + c_pendingActionFallbackTimeout);
        }

        auto now = std::chrono::steady_clock::now();
        auto delay = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now),
                              std::chrono::milliseconds(1));
        if (!m_pendingActionTimer) {
            auto dispatcher = DispatcherQueue();
            if (!dispatcher) return;
            m_pendingActionTimer = dispatcher.CreateTimer();
            m_pendingActionTimer.IsRepeating(false);
            auto weak = get_weak();
            m_pendingActionTimer.Tick([weak](auto const&, auto const&) noexcept {
                try {
                    if (auto self = weak.get(); self && self->m_presentationActive.load(std::memory_order_acquire)) {
                        self->RebuildDeviceListFromCache(true, true);
                    }
                } catch (winrt::hresult_error const& ex) {
                    util::DebugTraceException(L"[DevicePickerView] Pending-action timer failed", ex);
                } catch (std::exception const& ex) {
                    util::DebugTraceException(L"[DevicePickerView] Pending-action timer failed", ex);
                } catch (...) {
                    util::DebugTraceUnknownException(L"[DevicePickerView] Pending-action timer failed");
                }
            });
        } else {
            m_pendingActionTimer.Stop();
        }
        m_pendingActionTimer.Interval(delay);
        m_pendingActionTimer.Start();
    } catch (winrt::hresult_error const& ex) {
        util::DebugTraceException(L"[DevicePickerView] Failed to schedule pending-action expiry", ex);
    } catch (std::exception const& ex) {
        util::DebugTraceException(L"[DevicePickerView] Failed to schedule pending-action expiry", ex);
    } catch (...) {
        util::DebugTraceUnknownException(L"[DevicePickerView] Failed to schedule pending-action expiry");
    }
}

void DevicePickerView::StopPendingActionTimer() noexcept {
    auto timer = std::exchange(m_pendingActionTimer, nullptr);
    if (!timer) return;
    try {
        timer.Stop();
    } catch (...) {
    }
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
    m_loadDevicesCancelled.store(true);
    if (m_onClose) m_onClose();
}

void DevicePickerView::OnDeviceToggle(winrt::hstring const& id) {
    auto const& items = m_viewModel.RefreshSnapshot().Items;
    auto const device = std::ranges::find(items, std::wstring_view(id), &apc::device_picker::DeviceSnapshotItem::Id);
    if (device == items.end() || !device->IsAvailable || device->IsBusy) return;
    if (device->IsConnected) {
        OnDeviceDisconnectClicked(id);
        return;
    }
    if (!m_viewModel.CanSelect(id)) return;
    if (!BeginPendingDeviceAction(id)) return;

    if (m_onDeviceSelected) {
        m_onDeviceSelected(id);
    }
}

void DevicePickerView::OnDeviceDisconnectClicked(winrt::hstring const& id) {
    if (!BeginPendingDeviceAction(id)) return;
    if (m_onDeviceDisconnect) m_onDeviceDisconnect(id);
}

void DevicePickerView::OnDeviceReconnectClicked(winrt::hstring const& id) {
    if (!BeginPendingDeviceAction(id)) return;
    if (m_onDeviceReconnect) m_onDeviceReconnect(id);
}

void DevicePickerView::OnDisconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!BeginPendingGlobalAction()) return;
    if (m_onDisconnectAll) m_onDisconnectAll();
}

void DevicePickerView::OnReconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!BeginPendingGlobalAction()) return;
    if (m_onReconnectAll) m_onReconnectAll();
}
} // namespace winrt::AudioPlaybackConnector2::implementation
