#pragma once

#include <SettingsWindow.g.h>

namespace winrt::AudioPlaybackConnector2::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        SettingsWindow();

        void RootGrid_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void SetTargetPosition(int32_t x, int32_t y);

    private:
        void RebuildDeviceList();
        void StartWithWindowsToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget SyncStartupTaskStateAsync();
        winrt::fire_and_forget ApplyStartWithWindowsAsync(bool on);
        int32_t m_targetX = INT_MIN;
        int32_t m_targetY = INT_MIN;
        std::atomic_uint64_t m_startupRequestId = 0;
        bool m_suppressStartupToggle = false;
    };
}

namespace winrt::AudioPlaybackConnector2::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow>
    {
    };
}
