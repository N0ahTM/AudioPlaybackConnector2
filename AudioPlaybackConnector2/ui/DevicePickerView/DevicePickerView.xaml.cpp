#include <pch.h>
#include <ui/DevicePickerView/DevicePickerView.xaml.h>
#if __has_include("DevicePickerView.g.cpp")
#include <DevicePickerView.g.cpp>
#endif

#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <ui/ButtonHelpers.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace {
constexpr auto c_pendingActionFallbackTimeout = std::chrono::seconds(2);
constexpr double c_pickerMinWidth = 260.0;
constexpr double c_pickerMaxWidth = 520.0;
constexpr double c_pickerHorizontalChromeWidth = 64.0;
constexpr double c_connectedActionsWidth = 68.0;
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
    [[nodiscard]] static double WidthFor(std::vector<DevicePickerItemViewModel> const& items, bool showGlobalActions) {
        double maxNameWidth = 0.0;
        bool hasConnectedActions = false;
        for (auto const& item : items) {
            maxNameWidth = std::max(maxNameWidth, MeasureTextWidth(item.Name));
            hasConnectedActions = hasConnectedActions || item.IsConnected;
        }

        double desiredWidth = maxNameWidth + c_pickerHorizontalChromeWidth;
        if (hasConnectedActions) {
            desiredWidth += c_connectedActionsWidth;
        }
        if (showGlobalActions) {
            desiredWidth = std::max(desiredWidth, GlobalActionsWidth());
        }

        return std::clamp(desiredWidth, c_pickerMinWidth, c_pickerMaxWidth);
    }

private:
    [[nodiscard]] static double MeasureTextWidth(winrt::hstring const& text, double fontSize = 14.0) {
        auto block = TextBlock();
        block.Text(text);
        block.FontSize(fontSize);
        block.TextWrapping(TextWrapping::NoWrap);
        block.Measure({c_pickerMaxWidth * 2.0, 48.0});
        return block.DesiredSize().Width;
    }

    [[nodiscard]] static double GlobalActionsWidth() {
        auto disconnectAll = MeasureTextWidth(winrt::hstring(_("DisconnectAll")), 12.0);
        auto reconnectAll = MeasureTextWidth(winrt::hstring(_("ReconnectAll")), 12.0);
        return disconnectAll + reconnectAll + c_globalActionsChromeWidth;
    }
};
} // namespace

namespace winrt::AudioPlaybackConnector2::implementation {
/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

DevicePickerView::DevicePickerView() {
    InitializeComponent();
    BackdropElement().SystemBackdrop(Media::MicaBackdrop());
    auto weak = get_weak();
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
    DeviceList().SelectionChanged([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnDeviceSelected(sender, args);
        }
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::Initialize(std::shared_ptr<DeviceManager> manager,
                                  std::function<void()> onClose,
                                  std::function<void(winrt::hstring)> onDeviceSelected,
                                  std::function<void(winrt::hstring)> onDeviceDisconnect,
                                  std::function<void(winrt::hstring)> onDeviceReconnect,
                                  std::function<void()> onDisconnectAll,
                                  std::function<void()> onReconnectAll) {
    m_deviceManager = manager;
    m_viewModel.SetDeviceManager(manager);
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
}

void DevicePickerView::LoadDevices() {
    if (!m_viewModel.Empty()) {
        RebuildDeviceListFromCache();
    }

    if (m_isLoadingDevices) {
        SetRefreshIndicators(true, m_viewModel.Empty());
        return;
    }

    m_isLoadingDevices.store(true);
    m_loadDevicesCancelled.store(false);
    auto requestId = ++m_loadDevicesRequestId;
    m_activeLoadRequestId.store(requestId);

    const bool blockingRefresh = m_viewModel.Empty() && DeviceList().Items().Size() == 0;
    SetRefreshIndicators(true, blockingRefresh);

    auto dispatcher = this->DispatcherQueue();
    if (!dispatcher) {
        DebugTrace(L"[DevicePickerView] ERROR: no UI dispatcher available for LoadDevices");
        SetRefreshIndicators(false, false);
        m_isLoadingDevices.store(false);
        m_activeLoadRequestId.store(0);
        return;
    }

    auto manager = m_deviceManager.lock();
    if (!manager) {
        DebugTrace(L"[DevicePickerView] ERROR: no DeviceManager available for LoadDevices");
        OnDeviceEnumerationFailed(blockingRefresh, requestId);
        return;
    }

    auto weak = get_weak();

    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
        previousOp{nullptr};
    {
        std::lock_guard lock(m_refreshDevicesOpMutex);
        previousOp = std::exchange(m_refreshDevicesOp, nullptr);
    }
    CancelRefreshDevicesOperation(previousOp, L"LoadDevices");

    auto refreshOp = manager->RefreshDevicesAsync();
    {
        std::lock_guard lock(m_refreshDevicesOpMutex);
        m_refreshDevicesOp = refreshOp;
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
        pendingOp = refreshOp;

    pendingOp.Completed([weak, dispatcher, blockingRefresh, requestId](auto const& sender,
                                                                       winrt::Windows::Foundation::AsyncStatus status) {
        if (auto self = weak.get()) {
            std::lock_guard lock(self->m_refreshDevicesOpMutex);
            if (self->m_activeLoadRequestId.load() != requestId) {
                return;
            }
            self->m_refreshDevicesOp = nullptr;
        }

        if (status == winrt::Windows::Foundation::AsyncStatus::Canceled) {
            if (auto self = weak.get()) {
                if (self->m_loadDevicesRequestId.load() == requestId) {
                    self->m_isLoadingDevices.store(false);
                    self->m_activeLoadRequestId.store(0);
                }
            }
            return;
        }
        auto enqueueFailure = [&]() {
            bool enqueued = dispatcher.TryEnqueue([weak, blockingRefresh, requestId]() {
                if (auto self = weak.get()) self->OnDeviceEnumerationFailed(blockingRefresh, requestId);
            });
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
            bool enqueued = dispatcher.TryEnqueue([weak, devices, blockingRefresh, requestId]() {
                if (auto self = weak.get()) self->ApplyDeviceResults(devices, blockingRefresh, requestId);
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
}

void DevicePickerView::CancelLoadDevices() {
    m_loadDevicesCancelled.store(true);
    auto invalidatedRequest = ++m_loadDevicesRequestId;
    m_activeLoadRequestId.store(invalidatedRequest);
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

void DevicePickerView::RefreshDeviceStates() {
    if (m_viewModel.Empty()) return;
    RebuildDeviceListFromCache();
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Helpers ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::ApplyDeviceResults(
    winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
    bool blockingRefresh,
    uint64_t requestId) {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (m_activeLoadRequestId.load() != requestId) return;
    if (m_loadDevicesCancelled) {
        SetRefreshIndicators(false, false);
        m_isLoadingDevices.store(false);
        m_activeLoadRequestId.store(0);
        return;
    }
    SetRefreshIndicators(false, false);

    if (!devices) {
        if (blockingRefresh && m_viewModel.Empty() && DeviceList().Items().Size() == 0) {
            RebuildDeviceListFromCache();
        }
        m_isLoadingDevices.store(false);
        m_activeLoadRequestId.store(0);
        return;
    }

    m_viewModel.SetDevices(devices);
    RebuildDeviceListFromCache();
    m_isLoadingDevices.store(false);
    m_activeLoadRequestId.store(0);
}

void DevicePickerView::OnDeviceEnumerationFailed(bool blockingRefresh, uint64_t requestId) {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (m_activeLoadRequestId.load() != requestId) return;
    if (m_loadDevicesCancelled.load()) return;
    SetRefreshIndicators(false, false);
    m_isLoadingDevices.store(false);
    m_activeLoadRequestId.store(0);
    if (blockingRefresh && m_viewModel.Empty() && DeviceList().Items().Size() == 0) {
        RebuildDeviceListFromCache();
    }
}

void DevicePickerView::RebuildDeviceListFromCache(bool reconcilePendingActions) {
    m_suppressSelectionChanged.store(true);
    DeviceList().SelectedItem(nullptr);
    DeviceList().Items().Clear();

    int connectedCount = 0;
    if (auto manager = m_deviceManager.lock()) {
        connectedCount = static_cast<int>(manager->GetConnectedDevices().size());
    }

    std::vector<DevicePickerItemViewModel> items;
    if (!m_viewModel.Empty()) {
        items = m_viewModel.SnapshotItems();
        if (reconcilePendingActions) {
            ReconcilePendingActions(items);
        }
    }

    const bool anyBusy = std::any_of(items.begin(), items.end(), [](auto const& item) { return item.IsBusy; });
    ApplyGlobalActionState(connectedCount > 1, !anyBusy && !m_pendingGlobalAction);
    RootGrid().Width(DevicePickerSizer::WidthFor(items, connectedCount > 1));

    if (m_viewModel.Empty()) {
        auto emptyMsg = TextBlock();
        emptyMsg.Text(winrt::hstring(_("TrayMenu_NoDevices")));
        emptyMsg.Foreground(
            apc::ui::ThemeBrushOrFallback(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Colors::Gray()));
        DeviceList().Items().Append(emptyMsg);
    } else {
        for (auto const& device : items) {
            DeviceList().Items().Append(BuildDeviceListItem(device));
        }
    }

    DeviceList().SelectedItem(nullptr);
    m_suppressSelectionChanged.store(false);
}

ListViewItem DevicePickerView::BuildDeviceListItem(DevicePickerItemViewModel const& device) {
    auto item = ListViewItem();
    item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    const bool isBusy = device.IsBusy || m_pendingGlobalAction || IsDeviceActionPending(device.Id);

    auto grid = Grid();
    grid.HorizontalAlignment(HorizontalAlignment::Stretch);
    grid.ColumnSpacing(8);
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());

    auto nameTb = TextBlock();
    nameTb.Text(device.Name);
    nameTb.MinWidth(0);
    nameTb.VerticalAlignment(VerticalAlignment::Center);
    nameTb.TextTrimming(TextTrimming::CharacterEllipsis);
    nameTb.TextWrapping(TextWrapping::NoWrap);
    nameTb.MaxLines(1);
    apc::ui::SetTooltipText(nameTb, device.Name);
    Grid::SetColumn(nameTb, 0);

    auto infoPanel = StackPanel();
    infoPanel.Orientation(Orientation::Horizontal);
    infoPanel.HorizontalAlignment(HorizontalAlignment::Right);
    infoPanel.VerticalAlignment(VerticalAlignment::Center);
    infoPanel.Spacing(6);
    Grid::SetColumn(infoPanel, 1);

    if (isBusy) {
        item.IsEnabled(false);
        item.Opacity(0.6);

        auto busyRing = ProgressRing();
        busyRing.Width(14);
        busyRing.Height(14);
        busyRing.IsActive(true);
        busyRing.VerticalAlignment(VerticalAlignment::Center);
        infoPanel.Children().Append(busyRing);
    }

    if (device.IsConnected) {
        auto devId = device.Id;

        apc::ui::IconButtonOptions reconnectOptions;
        reconnectOptions.Foreground = apc::ui::TryThemeBrush(L"AccentFillColorDefaultBrush");
        auto reconnectBtn = apc::ui::CreateIconButton(L"\xE72C", winrt::hstring(_("Reconnect")), reconnectOptions);
        reconnectBtn.IsEnabled(!isBusy);
        auto weak = get_weak();
        reconnectBtn.Click([weak, devId](auto const&, auto const&) {
            if (auto self = weak.get()) self->OnDeviceReconnectClicked(devId);
        });

        apc::ui::IconButtonOptions disconnectOptions;
        disconnectOptions.Foreground = apc::ui::TryThemeBrush(L"SystemFillColorCriticalBrush");
        auto disconnectBtn = apc::ui::CreateIconButton(L"\xE711", winrt::hstring(_("Disconnect")), disconnectOptions);
        disconnectBtn.IsEnabled(!isBusy);
        disconnectBtn.Click([weak, devId](auto const&, auto const&) {
            if (auto self = weak.get()) self->OnDeviceDisconnectClicked(devId);
        });

        infoPanel.Children().Append(reconnectBtn);
        infoPanel.Children().Append(disconnectBtn);
    }

    grid.Children().Append(nameTb);
    grid.Children().Append(infoPanel);
    item.Content(grid);
    item.Tag(box_value(device.Id));
    return item;
}

bool DevicePickerView::BeginPendingDeviceAction(winrt::hstring const& id) {
    if (id.empty() || m_pendingGlobalAction || IsDeviceActionPending(id)) return false;
    m_pendingDeviceActions[std::wstring(id)] = std::chrono::steady_clock::now();
    RebuildDeviceListFromCache(false);
    return true;
}

bool DevicePickerView::BeginPendingGlobalAction() {
    if (m_pendingGlobalAction) return false;
    m_pendingGlobalAction = true;
    m_pendingGlobalActionStarted = std::chrono::steady_clock::now();
    RebuildDeviceListFromCache(false);
    return true;
}

bool DevicePickerView::IsDeviceActionPending(winrt::hstring const& id) const {
    return m_pendingDeviceActions.contains(std::wstring(id));
}

void DevicePickerView::ReconcilePendingActions(std::vector<DevicePickerItemViewModel> const& items) {
    auto const now = std::chrono::steady_clock::now();
    auto manager = m_deviceManager.lock();
    for (auto it = m_pendingDeviceActions.begin(); it != m_pendingDeviceActions.end();) {
        auto id = winrt::hstring(it->first);
        const bool managerOwnsBusy = manager && manager->IsDeviceBusy(id);
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
    m_loadDevicesCancelled.store(true);
    if (m_onClose) m_onClose();
}

void DevicePickerView::OnDeviceSelected(winrt::Windows::Foundation::IInspectable const&,
                                        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    if (m_suppressSelectionChanged.load()) return;
    auto selected = DeviceList().SelectedItem();
    if (!selected) return;

    auto lvi = selected.try_as<ListViewItem>();
    if (!lvi) return;

    auto tag = lvi.Tag();
    if (!tag) return;
    auto id = unbox_value<winrt::hstring>(tag);
    if (id.empty()) return;

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
