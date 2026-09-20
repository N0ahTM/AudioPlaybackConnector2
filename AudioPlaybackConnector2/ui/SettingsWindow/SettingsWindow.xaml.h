#pragma once

#include <core/StringResources.hpp>
#include <memory>

#include <util/Logger.hpp>

// Win32 aliases GetCurrentTime to GetTickCount; WinUI uses it as a method name.
#undef GetCurrentTime

#include <SettingsWindow.g.h>
#include <app/AppController.hpp>
#include <ui/SettingsDiagnosticsReport.hpp>
#include <ui/WindowPlacement.hpp>

#include <optional>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

namespace winrt::AudioPlaybackConnector2::implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    enum class InitializationState { Pending, Succeeded, Failed };

    SettingsWindow();
    ~SettingsWindow();

    void RootGrid_Loaded(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ResetWindowPlacementButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
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
    void SettingsHelpButton_Click(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsBackButton_Click(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsContentHost_SizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e);
    void LanguageComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
    void SetAppController(std::shared_ptr<apc::app::AppController> controller,
                          util::LogSink log,
                          std::shared_ptr<StringResources const> strings);
    void SetInitialSettingsSnapshot(SettingsData snapshot);
    void ApplyLanguage(std::wstring_view language);
    void SetDefaultPlacement(util::SettingsWindowPlacement placement);
    void ShowHelpPage();
    void SetTargetPlacement(util::SettingsWindowPlacement placement);
    [[nodiscard]] InitializationState InitializationStatus() const noexcept;

private:
    enum class SettingsPage { App, Help };

    static LRESULT CALLBACK SettingsWindowSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept;

    void InitializeSettingsContent();
    void CloseAfterInitializationFailure() noexcept;
    void LocalizeSettingsText();
    void ApplyCurrentWindowTheme(HWND hwnd) noexcept;
    void ApplySystemBackdropEffects(bool enabled) noexcept;
    void RevealAtTarget(HWND hwnd);
    void QueuePlacementSave();
    void CommitPlacementNow() noexcept;
    void StopPlacementSaveTimer() noexcept;
    bool StoreCurrentPlacement();
    void ResetWindowPlacement();
    void ApplyAdaptiveLayout();
    void ShowDiagnosticsInfo(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                             std::wstring_view title,
                             std::wstring_view message);
    winrt::fire_and_forget LaunchUri(std::wstring_view uri);
    void OpenLogFolder();
    bool CopyTextToClipboard(std::wstring_view text);
    void ShowSettingsPage(SettingsPage page);
    void StopPageTransition() noexcept;
    void SelectLanguage(std::wstring_view language);
    void UpdateContentPanelWidth();
    [[nodiscard]] util::SettingsWindowPlacement CalculateAdaptivePlacement();
    [[nodiscard]] double MeasureVisibleContentHeight(double contentWidth);
    [[nodiscard]] std::wstring BuildReportBugUri() const;
    void SetStartupTaskBusy(bool busy);
    void ApplyStartupTaskSnapshot(StartupTaskSnapshot const& snapshot) noexcept;
    static winrt::fire_and_forget CopyDiagnosticsAsync(winrt::weak_ref<SettingsWindow> weak,
                                                       winrt::apartment_context uiThread,
                                                       SettingsData settings,
                                                       std::size_t connectedDeviceCount,
                                                       std::filesystem::path logPath,
                                                       apc::ui::SettingsDiagnosticsReportContext context,
                                                       std::uint64_t requestId,
                                                       util::LogSink log);
    void StartWithWindowsToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    util::SettingsWindowPlacement m_defaultPlacement = util::CalculateSettingsWindowPlacement();
    util::SettingsWindowPlacement m_targetPlacement = util::CalculateSettingsWindowPlacement();
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_placementSaveTimer{nullptr};
    std::uint64_t m_lastStartupTaskPublication = 0;
    std::uint64_t m_diagnosticsCopyRequestId = 0;
    bool m_diagnosticsCopyInProgress = false;
    util::LogSink m_log;
    std::shared_ptr<StringResources const> m_strings;
    std::shared_ptr<apc::app::AppController> m_appController;
    apc::app::AppController::Subscription m_appSubscription;
    std::uint64_t m_lastAppRevision = 0;
    bool m_closed = false;
    std::optional<SettingsData> m_initialSettingsSnapshot;
    winrt::event_token m_actualThemeChangedToken{};
    SettingsPage m_currentPage = SettingsPage::App;
    winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_pageTransition{nullptr};
    bool m_suppressStartupToggle = false;
    bool m_suppressLanguageSelection = false;
    bool m_subclassInstalled = false;
    bool m_contentInitialized = false;
    bool m_capturePlacementChanges = false;
    bool m_loaded = false;
    bool m_hadPersistedPlacement = false;
    InitializationState m_initializationState = InitializationState::Pending;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
