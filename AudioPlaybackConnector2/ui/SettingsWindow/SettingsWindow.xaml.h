#pragma once

#include <SettingsWindow.g.h>
#include <services/SettingsController.hpp>
#include <services/UpdateService.hpp>
#include <ui/SettingsViewModel.hpp>
#include <util/Util.hpp>

namespace winrt::AudioPlaybackConnector2::implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    SettingsWindow();

    void RootGrid_Loaded(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void CheckForUpdatesButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OpenAppInstallerButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void SetSettingsController(std::shared_ptr<ISettingsController> controller);
    void SetTargetPlacement(util::SettingsWindowPlacement placement);

private:
    static LRESULT CALLBACK SettingsWindowSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    void InitializeSettingsContent();
    void RevealAtTarget(HWND hwnd);
    void RebuildDeviceList();
    void SetUpdateCheckBusy(bool busy);
    void ShowUpdateCheckResult(UpdateCheckResult const& result);
    void StartWithWindowsToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    winrt::fire_and_forget SyncStartupTaskStateAsync();
    winrt::fire_and_forget ApplyStartWithWindowsAsync(bool on);
    winrt::fire_and_forget RunManualUpdateCheckAsync();
    util::SettingsWindowPlacement m_targetPlacement = util::CalculateSettingsWindowPlacement();
    std::atomic_uint64_t m_startupRequestId = 0;
    std::atomic_uint64_t m_updateCheckRequestId = 0;
    std::shared_ptr<ISettingsController> m_settingsController;
    bool m_suppressStartupToggle = false;
    bool m_subclassInstalled = false;
    bool m_contentInitialized = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
