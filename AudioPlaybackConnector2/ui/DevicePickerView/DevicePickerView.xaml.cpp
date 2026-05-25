#include <pch.h>
#include <ui/DevicePickerView/DevicePickerView.xaml.h>
#if __has_include("DevicePickerView.g.cpp")
#include <DevicePickerView.g.cpp>
#endif

#include <core/DeviceManager.hpp>
#include <core/StringResources.hpp>
#include <util/Util.hpp>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::AudioPlaybackConnector2::implementation {
/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

DevicePickerView::DevicePickerView() {
    InitializeComponent();
    BackdropElement().SystemBackdrop(Media::MicaBackdrop());
    auto weak = get_weak();
    CloseButton().Click([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnCloseClicked(sender, args);
        }
    });
    DeviceList().SelectionChanged([weak](auto const& sender, auto const& args) {
        if (auto self = weak.get()) {
            self->OnDeviceSelected(sender, args);
        }
    });
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::Initialize(std::shared_ptr<DeviceManager> manager, std::function<void()> onClose, std::function<void(winrt::hstring)> onDeviceSelected, std::function<void(winrt::hstring)> onDeviceDisconnect, std::function<void(winrt::hstring)> onDeviceReconnect) {
    m_manager = manager;
    m_onClose = std::move(onClose);
    m_onDeviceSelected = std::move(onDeviceSelected);
    m_onDeviceDisconnect = std::move(onDeviceDisconnect);
    m_onDeviceReconnect = std::move(onDeviceReconnect);
    TitleText().Text(winrt::hstring(_("TrayMenu_SelectDevice")));
}

void DevicePickerView::LoadDevices() {
    if (m_isLoadingDevices) return;
    m_isLoadingDevices.store(true);
    m_loadDevicesCancelled.store(false);
    auto requestId = ++m_loadDevicesRequestId;

    bool listWasEmpty = DeviceList().Items().Size() == 0;
    if (listWasEmpty) {
        ProgressIndicator().Visibility(Visibility::Visible);
        ProgressIndicator().IsActive(true);
    }

    auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    auto selector = winrt::Windows::Media::Audio::AudioPlaybackConnection::GetDeviceSelector();
    auto weak = get_weak();

    if (m_findAllOp) {
        try {
            m_findAllOp.Cancel();
        } catch (...) {
        }
    }

    m_findAllOp = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);
    m_findAllOp.Completed([weak, dispatcher, listWasEmpty, requestId](auto const& sender, winrt::Windows::Foundation::AsyncStatus status) {
        if (status == winrt::Windows::Foundation::AsyncStatus::Canceled) {
            if (auto self = weak.get()) {
                if (self->m_loadDevicesRequestId.load() == requestId)
                    self->m_isLoadingDevices.store(false);
            }
            return;
        }
        try {
            auto devices = sender.GetResults();
            bool enqueued = dispatcher.TryEnqueue([weak, devices, listWasEmpty, requestId]() {
                if (auto self = weak.get())
                    self->ApplyDeviceResults(devices, listWasEmpty, requestId);
            });
            if (!enqueued) {
                if (auto self = weak.get()) {
                    if (self->m_loadDevicesRequestId.load() == requestId)
                        self->m_isLoadingDevices.store(false);
                }
            }
        } catch (...) {
            DebugTrace(L"[DevicePickerView] ERROR: FindAllAsync failed");
            bool enqueued = dispatcher.TryEnqueue([weak, listWasEmpty, requestId]() {
                if (auto self = weak.get())
                    self->OnDeviceEnumerationFailed(listWasEmpty, requestId);
            });
            if (!enqueued) {
                if (auto self = weak.get()) {
                    if (self->m_loadDevicesRequestId.load() == requestId)
                        self->m_isLoadingDevices.store(false);
                }
            }
        }
    });
}

void DevicePickerView::CancelLoadDevices() {
    m_loadDevicesCancelled.store(true);
    ++m_loadDevicesRequestId;
    m_isLoadingDevices.store(false);
    if (m_findAllOp) {
        try {
            m_findAllOp.Cancel();
        } catch (...) {
        }
        m_findAllOp = nullptr;
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Private Helpers /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::ApplyDeviceResults(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices, bool listWasEmpty, uint64_t requestId) {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (m_loadDevicesCancelled) {
        if (listWasEmpty) {
            ProgressIndicator().IsActive(false);
            ProgressIndicator().Visibility(Visibility::Collapsed);
        }
        m_isLoadingDevices.store(false);
        return;
    }
    ProgressIndicator().IsActive(false);
    ProgressIndicator().Visibility(Visibility::Collapsed);
    DeviceList().Items().Clear();

    if (devices.Size() == 0) {
        auto emptyMsg = TextBlock();
        emptyMsg.Text(winrt::hstring(_("TrayMenu_NoDevices")));
        auto brush = Application::Current().Resources().TryLookup(box_value(L"TextFillColorSecondaryBrush"));
        if (brush) {
            emptyMsg.Foreground(brush.as<Media::SolidColorBrush>());
        } else {
            emptyMsg.Foreground(Media::SolidColorBrush(winrt::Windows::UI::Colors::Gray()));
        }
        DeviceList().Items().Append(emptyMsg);
    } else {
        for (auto const& dev : devices) {
            DeviceList().Items().Append(BuildDeviceListItem(dev));
        }
    }
    m_isLoadingDevices.store(false);
}

void DevicePickerView::OnDeviceEnumerationFailed(bool listWasEmpty, uint64_t requestId) {
    if (m_loadDevicesRequestId.load() != requestId) return;
    if (listWasEmpty) {
        ProgressIndicator().IsActive(false);
        ProgressIndicator().Visibility(Visibility::Collapsed);
    }
    m_isLoadingDevices.store(false);
}

ListViewItem DevicePickerView::BuildDeviceListItem(winrt::Windows::Devices::Enumeration::DeviceInformation const& dev) {
    auto item = ListViewItem();
    auto grid = Grid();
    grid.ColumnDefinitions().Append(ColumnDefinition());
    grid.ColumnDefinitions().Append(ColumnDefinition());

    auto nameTb = TextBlock();
    nameTb.Text(dev.Name());
    nameTb.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(nameTb, 0);

    auto infoPanel = StackPanel();
    infoPanel.Orientation(Orientation::Horizontal);
    infoPanel.HorizontalAlignment(HorizontalAlignment::Right);
    infoPanel.VerticalAlignment(VerticalAlignment::Center);
    infoPanel.Spacing(4);
    Grid::SetColumn(infoPanel, 1);

    auto manager = m_manager.lock();
    bool isConnected = false;
    if (manager) {
        for (const auto& c : manager->GetConnectedDevices()) {
            if (c.Device.Id() == dev.Id()) {
                isConnected = true;
                break;
            }
        }
    }

    if (isConnected) {
        auto devId = dev.Id();
        auto onReconnect = m_onDeviceReconnect;
        auto onDisconnect = m_onDeviceDisconnect;

        auto reconnectBtn = Button();
        reconnectBtn.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        reconnectBtn.BorderThickness({0});
        reconnectBtn.Padding({5, 1, 5, 1});
        reconnectBtn.VerticalAlignment(VerticalAlignment::Center);

        auto reconnectText = TextBlock();
        reconnectText.Text(winrt::hstring(_("Reconnect")));
        reconnectText.FontSize(11);
        if (auto brush = Application::Current().Resources().TryLookup(box_value(L"AccentFillColorDefaultBrush"))) {
            reconnectText.Foreground(brush.as<Media::Brush>());
        }
        reconnectBtn.Content(reconnectText);
        reconnectBtn.Click([onReconnect, devId](auto const&, auto const&) {
            if (onReconnect) onReconnect(devId);
        });

        auto disconnectBtn = Button();
        disconnectBtn.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        disconnectBtn.BorderThickness({0});
        disconnectBtn.Padding({5, 1, 5, 1});
        disconnectBtn.VerticalAlignment(VerticalAlignment::Center);

        auto disconnectText = TextBlock();
        disconnectText.Text(winrt::hstring(_("Disconnect")));
        disconnectText.FontSize(11);
        if (auto brush = Application::Current().Resources().TryLookup(box_value(L"SystemFillColorCriticalBrush"))) {
            disconnectText.Foreground(brush.as<Media::Brush>());
        }
        disconnectBtn.Content(disconnectText);
        disconnectBtn.Click([onDisconnect, devId](auto const&, auto const&) {
            if (onDisconnect) onDisconnect(devId);
        });

        infoPanel.Children().Append(reconnectBtn);
        infoPanel.Children().Append(disconnectBtn);
    }

    grid.Children().Append(nameTb);
    grid.Children().Append(infoPanel);
    item.Content(grid);
    item.Tag(box_value(dev.Id()));
    return item;
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Event Handlers ///////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void DevicePickerView::OnCloseClicked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
    m_loadDevicesCancelled.store(true);
    if (m_onClose) m_onClose();
}

void DevicePickerView::OnDeviceSelected(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    auto selected = DeviceList().SelectedItem();
    if (!selected) return;

    auto lvi = selected.try_as<ListViewItem>();
    if (!lvi) return;

    auto tag = lvi.Tag();
    if (!tag) return;
    auto id = unbox_value<winrt::hstring>(tag);
    if (id.empty()) return;

    auto manager = m_manager.lock();
    if (manager) {
        for (const auto& c : manager->GetConnectedDevices()) {
            if (c.Device.Id() == id) return;
        }
    }

    if (m_onDeviceSelected) {
        m_onDeviceSelected(id);
    }
}
} // namespace winrt::AudioPlaybackConnector2::implementation
