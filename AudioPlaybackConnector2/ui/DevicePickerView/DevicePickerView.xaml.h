#pragma once

#include <DevicePickerView.g.h>
#include <mutex>
#include <vector>

class DeviceManager;

namespace winrt::Microsoft::UI::Xaml {
struct RoutedEventArgs;
}
namespace winrt::Microsoft::UI::Xaml::Controls {
struct SelectionChangedEventArgs;
struct ListViewItem;
} // namespace winrt::Microsoft::UI::Xaml::Controls
namespace winrt::Windows::Devices::Enumeration {
struct DeviceInformation;
struct DeviceInformationCollection;
} // namespace winrt::Windows::Devices::Enumeration

namespace winrt::AudioPlaybackConnector2::implementation {
struct DevicePickerView : DevicePickerViewT<DevicePickerView> {
    DevicePickerView();
    void Initialize(std::shared_ptr<DeviceManager> manager,
                    std::function<void()> onClose,
                    std::function<void(winrt::hstring)> onDeviceSelected,
                    std::function<void(winrt::hstring)> onDeviceDisconnect = nullptr,
                    std::function<void(winrt::hstring)> onDeviceReconnect = nullptr);
    void LoadDevices();
    void CancelLoadDevices();
    void RefreshDeviceStates();

private:
    void OnCloseClicked(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnDeviceSelected(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

    void ApplyDeviceResults(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
                            bool listWasEmpty,
                            uint64_t requestId);
    void OnDeviceEnumerationFailed(bool listWasEmpty, uint64_t requestId);
    void RebuildDeviceListFromCache();
    winrt::Microsoft::UI::Xaml::Controls::ListViewItem
    BuildDeviceListItem(winrt::Windows::Devices::Enumeration::DeviceInformation const& dev);

    std::weak_ptr<DeviceManager> m_manager;
    std::function<void()> m_onClose;
    std::function<void(winrt::hstring)> m_onDeviceSelected;
    std::function<void(winrt::hstring)> m_onDeviceDisconnect;
    std::function<void(winrt::hstring)> m_onDeviceReconnect;
    std::atomic<bool> m_isLoadingDevices = false;
    std::atomic<bool> m_loadDevicesCancelled = false;
    std::atomic<bool> m_suppressSelectionChanged = false;
    std::atomic<uint64_t> m_loadDevicesRequestId = 0;
    std::atomic<uint64_t> m_activeLoadRequestId = 0;
    mutable std::mutex m_findAllOpMutex;
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
        m_findAllOp{nullptr};
    std::vector<winrt::Windows::Devices::Enumeration::DeviceInformation> m_devices;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct DevicePickerView : DevicePickerViewT<DevicePickerView, implementation::DevicePickerView> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
