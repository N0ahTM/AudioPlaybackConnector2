#include <pch.h>
#include <ui/App/App.xaml.h>
#if __has_include("App.g.cpp")
#include <App.g.cpp>
#endif

#include <app/ApplicationHost.hpp>
#include <util/Logger.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Constructors / Destructor /////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

winrt::AudioPlaybackConnector2::implementation::App::App() = default;

winrt::AudioPlaybackConnector2::implementation::App::~App() {
    if (m_host) {
        m_host->Shutdown();
    }
}

void winrt::AudioPlaybackConnector2::implementation::App::RegisterUnhandledExceptionHandler() {
    if (m_unhandledExceptionHandlerRegistered) return;
    m_unhandledExceptionHandlerRegistered = true;

    UnhandledException([](winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args) noexcept {
        try {
            DebugTrace(L"[App] XAML unhandled exception: 0x{0:08X} {1}",
                       static_cast<uint32_t>(args.Exception()),
                       args.Message());
            util::FlushInMemoryLogTailToFile(L"xaml-unhandled-exception", static_cast<uint32_t>(args.Exception()));
        } catch (...) {
        }
    });
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Launch ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void winrt::AudioPlaybackConnector2::implementation::App::OnLaunched(
    [[maybe_unused]] Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e) {
    if (!m_host) {
        m_host = std::make_shared<ApplicationHost>();
    }
    RegisterUnhandledExceptionHandler();
    m_host->Start();
}
