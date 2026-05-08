#pragma once

#include <MainWindow.g.h>

namespace winrt::AudioPlaybackConnector2::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow() = default;
};
} // namespace winrt::AudioPlaybackConnector2::implementation

namespace winrt::AudioPlaybackConnector2::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {
};
} // namespace winrt::AudioPlaybackConnector2::factory_implementation
