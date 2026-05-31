#include <pch.h>
#include <ui/App/App.xaml.h>
#if __has_include("App.g.cpp")
#include <App.g.cpp>
#endif

#include <app/ApplicationHost.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::AudioPlaybackConnector2::implementation::App::App() : m_host(std::make_shared<ApplicationHost>()) {}

winrt::AudioPlaybackConnector2::implementation::App::~App() {
    if (m_host) {
        m_host->Shutdown();
    }
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnLaunched(
    [[maybe_unused]] Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e) {
    if (!m_host) {
        m_host = std::make_shared<ApplicationHost>();
    }
    m_host->Start();
}
