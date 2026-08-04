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
    [[nodiscard]] static double WidthFor(std::vector<apc::device_picker::DeviceSnapshotItem> const& items,
                                         bool showGlobalActions) {
        double maxNameWidth = 0.0;
        bool hasConnectedActions = false;
        for (auto const& item : items) {
            maxNameWidth = std::max(maxNameWidth, MeasureTextWidth(item.DisplayName));
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
                                  std::shared_ptr<Settings> settings,
                                  std::function<void()> onClose,
                                  std::function<void(winrt::hstring)> onDeviceSelected,
                                  std::function<void(winrt::hstring)> onDeviceDisconnect,
                                  std::function<void(winrt::hstring)> onDeviceReconnect,
                                  std::function<void()> onDisconnectAll,
                                  std::function<void()> onReconnectAll) {
    m_preparedForRelease.store(false);
    m_presentationActive.store(false, std::memory_order_release);
    m_deviceManager = manager;
    m_settings = settings;
    m_viewModel.SetDeviceManager(manager);
    m_viewModel.SetSettings(settings);
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

bool DevicePickerView::LoadDevices() {
    if (m_preparedForRelease.load()) {
        return false;
    }

    if (m_isLoadingDevices) {
        SetRefreshIndicators(true, !m_viewModel.HasInventory());
        return true;
    }

    const bool nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromManager();
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

    auto manager = m_deviceManager.lock();
    if (!manager) {
        DebugTrace(L"[DevicePickerView] ERROR: no DeviceManager available for LoadDevices");
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
        auto refreshOp = manager->RefreshDevicesAsync();
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
                if (auto self = weak.get()) {
                    if (self->m_loadDevicesRequestId.load() == requestId) {
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
    m_deviceManager.reset();
    m_settings.reset();
    m_pendingDeviceActions.clear();
    m_pendingGlobalAction = false;
    m_renderedSnapshotGeneration = 0;
    m_hasRenderedSnapshot = false;
    m_viewModel.Clear();
}

void DevicePickerView::RefreshDeviceStates() {
    if (!m_viewModel.HasInventory()) return;
    RebuildDeviceListFromCache();
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
            nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromManager();
            if (m_viewModel.HasInventory() || (blockingRefresh && DeviceList().Items().Size() == 0)) {
                RebuildDeviceListFromCache();
            }
        } else {
            nativeInventoryComplete = m_viewModel.SynchronizeInventoryFromManager();
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

    m_suppressSelectionChanged.store(true);
    DeviceList().SelectedItem(nullptr);
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
    RootGrid().Width(DevicePickerSizer::WidthFor(items, connectedCount > 1));

    if (items.empty()) {
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
    m_renderedSnapshotGeneration = snapshot.Generation;
    m_hasRenderedSnapshot = true;
    SchedulePendingActionExpiry();
}

ListViewItem DevicePickerView::BuildDeviceListItem(apc::device_picker::DeviceSnapshotItem const& device) {
    auto item = ListViewItem();
    item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    const bool isBusy = device.IsBusy || m_pendingGlobalAction || IsDeviceActionPending(winrt::hstring(device.Id));

    auto grid = Grid();
    grid.HorizontalAlignment(HorizontalAlignment::Stretch);
    grid.ColumnSpacing(8);
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());

    auto nameTb = TextBlock();
    nameTb.Text(winrt::hstring(device.DisplayName));
    nameTb.MinWidth(0);
    nameTb.VerticalAlignment(VerticalAlignment::Center);
    nameTb.TextTrimming(TextTrimming::CharacterEllipsis);
    nameTb.TextWrapping(TextWrapping::NoWrap);
    nameTb.MaxLines(1);
    apc::ui::SetTooltipText(nameTb, winrt::hstring(device.DisplayName));
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
        auto devId = winrt::hstring(device.Id);

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
    item.Tag(box_value(winrt::hstring(device.Id)));
    return item;
}

bool DevicePickerView::BeginPendingDeviceAction(winrt::hstring const& id) {
    if (id.empty() || m_pendingGlobalAction || IsDeviceActionPending(id)) return false;
    m_pendingDeviceActions[std::wstring(id)] = std::chrono::steady_clock::now();
    RebuildDeviceListFromCache(false, true);
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
