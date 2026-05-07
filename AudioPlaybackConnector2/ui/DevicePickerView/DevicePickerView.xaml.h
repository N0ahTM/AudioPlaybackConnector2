#pragma once

#include <DevicePickerView.g.h>

class DeviceManager;

namespace winrt::Microsoft::UI::Xaml { struct RoutedEventArgs; }
namespace winrt::Microsoft::UI::Xaml::Controls { struct SelectionChangedEventArgs; }

namespace winrt::AudioPlaybackConnector2::implementation
{
    struct DevicePickerView : DevicePickerViewT<DevicePickerView>
    {
        DevicePickerView();
        void Initialize(std::shared_ptr<DeviceManager> manager, std::function<void()> onClose, std::function<void(winrt::hstring)> onDeviceSelected, std::function<void(winrt::hstring)> onDeviceDisconnect = nullptr, std::function<void(winrt::hstring)> onDeviceReconnect = nullptr);
        void LoadDevices();
        void CancelLoadDevices();

    private:
        void OnCloseClicked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDeviceSelected(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        std::weak_ptr<DeviceManager> m_manager;
        std::function<void()> m_onClose;
        std::function<void(winrt::hstring)> m_onDeviceSelected;
        std::function<void(winrt::hstring)> m_onDeviceDisconnect;
        std::function<void(winrt::hstring)> m_onDeviceReconnect;
        std::atomic<bool> m_isLoadingDevices = false;
        std::atomic<bool> m_loadDevicesCancelled = false;
        std::atomic<uint64_t> m_loadDevicesRequestId = 0;
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection> m_findAllOp{ nullptr };
    };
}

namespace winrt::AudioPlaybackConnector2::factory_implementation
{
    struct DevicePickerView : DevicePickerViewT<DevicePickerView, implementation::DevicePickerView>
    {
    };
}
