#pragma once

#include <ui/App/App.xaml.g.h>
#include <util/Logger.hpp>
#include <util/CrashHandler.hpp>

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

    util::Logger m_logger;
    util::crash::CrashHandlers m_crashHandlers{m_logger.Emergency()};
    std::shared_ptr<ApplicationHost> m_host;
    bool m_unhandledExceptionHandlerRegistered = false;
};
} // namespace winrt::AudioPlaybackConnector2::implementation
