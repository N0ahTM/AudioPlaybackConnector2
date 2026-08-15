#pragma once

#include <SettingsWindow.g.h>
#include <app/StartupTaskCoordinator.hpp>
#include <services/SettingsController.hpp>
#include <services/UpdateService.hpp>
#include <ui/SettingsViewModel.hpp>
#include <ui/WindowPlacement.hpp>

class UpdateCoordinator;

namespace winrt::AudioPlaybackConnector2::implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    SettingsWindow();
    ~SettingsWindow();

    void RootGrid_Loaded(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void CheckForUpdatesButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OpenAppInstallerButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ResetWindowPlacementButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void DefaultLastConnectedButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void RepositoryButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void FeatureRequestButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void TroubleshootingButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OpenBluetoothSettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OpenLogFolderButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void CopyDiagnosticsButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ReportBugButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void StreamDeckSetupButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void SettingsNavigation_SelectionChanged(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& e);
    void SettingsContentHost_SizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e);
    void LanguageComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
    void SetSettingsController(std::shared_ptr<ISettingsController> controller);
    void SetStartupTaskCoordinator(std::shared_ptr<StartupTaskCoordinator> coordinator);
    void SetInitialSettingsSnapshot(SettingsData snapshot);
    void SetUpdateCoordinator(std::shared_ptr<UpdateCoordinator> coordinator);
    void SetDefaultPlacement(util::SettingsWindowPlacement placement);
    void SetTargetPlacement(util::SettingsWindowPlacement placement);
    void RefreshKnownDevices();

private:
    enum class SettingsPage { Devices, App, Privacy, StreamDeck, Help, About };

    static LRESULT CALLBACK SettingsWindowSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept;

    void InitializeSettingsContent();
    void LocalizeSettingsText();
    void ApplyCurrentWindowTheme(HWND hwnd) noexcept;
    void ApplySystemBackdropEffects(bool enabled) noexcept;
    void RevealAtTarget(HWND hwnd);
    void QueuePlacementSave();
    void SavePlacementNow() noexcept;
    void StopPlacementSaveTimer() noexcept;
    bool StoreCurrentPlacement();
    void ResetWindowPlacement();
    void ApplyAdaptiveLayout();
    void RequestDeviceListRebuild(bool force = false);
    void ProcessPendingDeviceListRebuild() noexcept;
    void RebuildDeviceList(bool force = false);
    void ShowDiagnosticsInfo(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                             std::wstring_view title,
                             std::wstring_view message);
    winrt::fire_and_forget LaunchUri(std::wstring_view uri);
    void OpenLogFolder();
    bool CopyTextToClipboard(std::wstring_view text);
    void ShowSettingsPage(SettingsPage page);
    void SelectLanguage(std::wstring_view language);
    void UpdateContentPanelWidth();
    [[nodiscard]] util::SettingsWindowPlacement CalculateAdaptivePlacement();
    [[nodiscard]] double CalculateNavigationPaneLength();
    [[nodiscard]] double MeasureVisibleContentHeight(double contentWidth);
    void CommitAlias(std::wstring const& deviceId, winrt::Microsoft::UI::Xaml::Controls::TextBox const& textBox);
    [[nodiscard]] std::wstring BuildReportBugUri() const;
    void SetUpdateCheckBusy(bool busy);
    void SetStartupTaskBusy(bool busy);
    void ApplyStartupTaskSnapshot(StartupTaskSnapshot const& snapshot) noexcept;
    void ShowUpdateCheckResult(UpdateCheckResult const& result);
    void StartWithWindowsToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    winrt::fire_and_forget RunManualUpdateCheckAsync();
    winrt::fire_and_forget ClearAliasSavedAfterDelayAsync(uint64_t requestId);
    util::SettingsWindowPlacement m_defaultPlacement = util::CalculateSettingsWindowPlacement();
    util::SettingsWindowPlacement m_targetPlacement = util::CalculateSettingsWindowPlacement();
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_placementSaveTimer{nullptr};
    std::atomic_uint64_t m_aliasSavedRequestId = 0;
    std::atomic_uint64_t m_updateCheckRequestId = 0;
    std::uint64_t m_lastStartupTaskPublication = 0;
    std::shared_ptr<ISettingsController> m_settingsController;
    std::shared_ptr<StartupTaskCoordinator> m_startupTaskCoordinator;
    StartupTaskCoordinator::HandlerToken m_startupTaskHandlerToken = 0;
    std::shared_ptr<UpdateCoordinator> m_updateCoordinator;
    std::wstring m_aliasSavedDeviceId;
    std::wstring m_renderedAliasSavedDeviceId;
    std::vector<SettingsDeviceViewModel> m_renderedDeviceItems;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_aliasSavedIndicator{nullptr};
    std::optional<SettingsData> m_initialSettingsSnapshot;
    winrt::event_token m_actualThemeChangedToken{};
    SettingsPage m_currentPage = SettingsPage::Devices;
    bool m_suppressStartupToggle = false;
    bool m_suppressLanguageSelection = false;
    bool m_suppressNavigationSelection = false;
    bool m_subclassInstalled = false;
    bool m_contentInitialized = false;
    bool m_deviceListRebuildQueued = false;
    bool m_deviceListRebuildPending = false;
    bool m_forceDeviceListRebuildPending = false;
    bool m_hasDeviceListRenderState = false;
    bool m_renderedGlobalConnectOnStartup = false;
    bool m_renderedGlobalReconnectOnConnectionLoss = false;
    bool m_renderedDefaultLastConnectedEnabled = false;
    bool m_capturePlacementChanges = false;
    bool m_adaptiveLayoutReady = false;
    bool m_loaded = false;
    bool m_hadPersistedPlacement = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
