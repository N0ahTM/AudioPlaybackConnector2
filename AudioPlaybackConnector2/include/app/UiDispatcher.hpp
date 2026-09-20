#pragma once

#include <util/Logger.hpp>
#include <control/ControlUiActionGate.hpp>
#include <chrono>
#include <stop_token>
#include <windows.h>
#include <functional>
#include <memory>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UI Dispatcher /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Construct, handle messages, stop and destroy on the window's UI thread. Dispatch must
// enqueue asynchronously; false/exception means nothing was queued. The window
// remains valid until Stop returns. Run may be called from any thread.
class UiDispatcher {
public:
    using ActionResult = apc::control::ControlUiActionGate::Result;
    using Task = std::function<void()>;
    using Dispatch = std::function<bool(Task)>;
    UiDispatcher(HWND window, Dispatch dispatch, util::LogSink log = {});
    ~UiDispatcher();
    UiDispatcher(UiDispatcher const&) = delete;
    UiDispatcher& operator=(UiDispatcher const&) = delete;

    [[nodiscard]] bool Run(Task task) noexcept;
    [[nodiscard]] ActionResult
    RunAndWait(std::function<bool()> action, std::stop_token stop, std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool HandleMessage(UINT message) noexcept;
    // Cancels queued work and drains only admitted native posts, never UI work.
    void Stop() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
