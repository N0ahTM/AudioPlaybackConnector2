#pragma once

#include <core/StringResources.hpp>
#include <memory>

#include <util/Logger.hpp>

#include <app/AppController.hpp>
#include <DevicePickerView.g.h>
#include <ui/DevicePickerViewState.hpp>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <chrono>
#include <stop_token>
#include <memory>
#include <unordered_map>

namespace winrt::Microsoft::UI::Xaml {
struct RoutedEventArgs;
}
namespace winrt::Microsoft::UI::Xaml::Controls {
struct SelectionChangedEventArgs;
struct ListViewItem;
} // namespace winrt::Microsoft::UI::Xaml::Controls
namespace winrt::AudioPlaybackConnector2::implementation {
struct DevicePickerView : DevicePickerViewT<DevicePickerView> {
    DevicePickerView();
    ~DevicePickerView();
    void Initialize(std::weak_ptr<apc::app::AppController> controller,
                    std::function<void()> onClose,
                    std::function<void()> showSettings,
                    util::LogSink log,
                    std::shared_ptr<StringResources const> strings);
    [[nodiscard]] bool LoadDevices();
    void CancelLoadDevices();
    void PrepareForRelease() noexcept;
    void RefreshDeviceStates();
    void ApplyLanguage();
    [[nodiscard]] bool InvalidateDeviceInventory();
    void SetPresentationActive(bool active) noexcept;

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

    static winrt::fire_and_forget RefreshDevicesAsync(winrt::weak_ref<DevicePickerView> weak,
                                                      winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                                      std::shared_ptr<apc::app::AppController> controller,
                                                      std::stop_token stop,
                                                      util::LogSink log);
    [[nodiscard]] std::optional<DeviceOptionsViewModel> DeviceOptions(std::wstring_view id) const;
    void RenderDeviceList(bool reconcilePendingActions = true, bool forceRender = false);
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

    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    std::weak_ptr<apc::app::AppController> m_appController;
    DevicePickerViewState m_viewState;
    std::function<void()> m_onClose;
    std::function<void()> m_onShowSettings;
    std::wstring m_optionsDeviceId;
    std::wstring m_savedAlias;
    bool m_updatingDeviceOptions = false;
    bool m_savedDevicesExpanded = false;
    winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_navigationAnimation{nullptr};
    std::uint64_t m_navigationAnimationGeneration = 0;
    // The view is UI-thread-owned; background refresh only holds a stop token and a weak window reference.
    bool m_isLoadingDevices = false;
    std::stop_source m_refreshCancellation;
    bool m_presentationActive = false;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_pendingDeviceActions;
    std::chrono::steady_clock::time_point m_pendingGlobalActionStarted{};
    bool m_pendingGlobalAction = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_pendingActionTimer{nullptr};
    std::uint64_t m_renderedSnapshotGeneration = 0;
    bool m_hasRenderedSnapshot = false;
    bool m_preparedForRelease = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct DevicePickerView : DevicePickerViewT<DevicePickerView, implementation::DevicePickerView> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
