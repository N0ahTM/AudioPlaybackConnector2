#pragma once

#include <DevicePickerView.g.h>
#include <ui/DevicePickerViewModel.hpp>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

class SettingsStore;

namespace apc::device {
class DeviceService;
}

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
    void Initialize(std::shared_ptr<apc::device::DeviceService> service,
                    std::shared_ptr<SettingsStore> settingsStore,
                    std::function<void()> onClose,
                    std::function<void(winrt::hstring)> onDeviceSelected,
                    std::function<void(winrt::hstring)> onDeviceDisconnect = nullptr,
                    std::function<void(winrt::hstring)> onDeviceReconnect = nullptr,
                    std::function<void()> onDisconnectAll = nullptr,
                    std::function<void()> onReconnectAll = nullptr);
    [[nodiscard]] bool LoadDevices();
    void CancelLoadDevices();
    void PrepareForRelease() noexcept;
    void RefreshDeviceStates();
    void ApplyLanguage();
    [[nodiscard]] bool InvalidateDeviceInventory();
    void SetPresentationActive(bool active) noexcept;
    void SetDeviceSettings(std::shared_ptr<ISettingsController> controller,
                           apc::app::SettingsWindowCommandExecutor::ExecuteCallback execute,
                           std::function<void()> showSettings);

private:
    void OnCloseClicked(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnDeviceToggle(winrt::hstring const& id);
    void ShowDeviceOptions(std::wstring const& id);
    void ReturnToDeviceList();
    void AnimateNavigation(double previousHeight) noexcept;
    void StopNavigationAnimation() noexcept;
    void RefreshDeviceOptions(bool resetAlias = false);
    [[nodiscard]] bool SaveDeviceAlias();
    void ShowDeviceOptionsError();
    void OnDisconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnReconnectAllClicked(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnDeviceDisconnectClicked(winrt::hstring const& id);
    void OnDeviceReconnectClicked(winrt::hstring const& id);

    void ApplyDeviceResults(winrt::Windows::Devices::Enumeration::DeviceInformationCollection const& devices,
                            bool blockingRefresh,
                            uint64_t requestId,
                            uint64_t inventoryGenerationAtStart) noexcept;
    void
    OnDeviceEnumerationFailed(bool blockingRefresh, uint64_t requestId, uint64_t inventoryGenerationAtStart) noexcept;
    void RebuildDeviceListFromCache(bool reconcilePendingActions = true, bool forceRender = false);
    winrt::Microsoft::UI::Xaml::Controls::ListViewItem
    BuildDeviceListItem(apc::device_picker::DeviceSnapshotItem const& device);
    bool BeginPendingDeviceAction(winrt::hstring const& id);
    bool BeginPendingGlobalAction();
    bool IsDeviceActionPending(winrt::hstring const& id) const;
    void ReconcilePendingActions(std::vector<apc::device_picker::DeviceSnapshotItem> const& items);
    void SchedulePendingActionExpiry() noexcept;
    void StopPendingActionTimer() noexcept;
    void ApplyGlobalActionState(bool visible, bool enabled);
    void SetRefreshIndicators(bool refreshing, bool blockingRefresh);

    DevicePickerViewModel m_viewModel;
    std::function<void()> m_onClose;
    std::function<void(winrt::hstring)> m_onDeviceSelected;
    std::function<void(winrt::hstring)> m_onDeviceDisconnect;
    std::function<void(winrt::hstring)> m_onDeviceReconnect;
    std::function<void()> m_onDisconnectAll;
    std::function<void()> m_onReconnectAll;
    std::function<void()> m_onShowSettings;
    std::wstring m_optionsDeviceId;
    std::wstring m_savedAlias;
    bool m_updatingDeviceOptions = false;
    bool m_savedDevicesExpanded = false;
    winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_navigationAnimation{nullptr};
    std::uint64_t m_navigationAnimationGeneration = 0;
    std::weak_ptr<apc::device::DeviceService> m_deviceService;
    std::weak_ptr<SettingsStore> m_settingsStore;
    std::atomic<bool> m_isLoadingDevices = false;
    std::atomic<bool> m_loadDevicesCancelled = false;
    std::atomic<uint64_t> m_loadDevicesRequestId = 0;
    std::atomic<uint64_t> m_activeLoadRequestId = 0;
    std::shared_ptr<apc::device_picker::DeviceInventoryGeneration> m_inventoryGeneration =
        std::make_shared<apc::device_picker::DeviceInventoryGeneration>();
    std::atomic_bool m_presentationActive = false;
    mutable std::mutex m_refreshDevicesOpMutex;
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
        m_refreshDevicesOp{nullptr};
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_pendingDeviceActions;
    std::chrono::steady_clock::time_point m_pendingGlobalActionStarted{};
    bool m_pendingGlobalAction = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_pendingActionTimer{nullptr};
    std::uint64_t m_renderedSnapshotGeneration = 0;
    bool m_hasRenderedSnapshot = false;
    std::atomic<bool> m_preparedForRelease = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct DevicePickerView : DevicePickerViewT<DevicePickerView, implementation::DevicePickerView> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
