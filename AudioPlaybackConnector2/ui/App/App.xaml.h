#pragma once

#include <ui/App/App.xaml.g.h>

class ApplicationHost;

namespace winrt::AudioPlaybackConnector2::implementation {
struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e);

private:
    void RegisterUnhandledExceptionHandler();

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    std::shared_ptr<ApplicationHost> m_host;
    bool m_unhandledExceptionHandlerRegistered = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation
