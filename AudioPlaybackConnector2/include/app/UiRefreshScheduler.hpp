#pragma once

#include <app/UiRefreshCoalescer.hpp>
#include <util/Logger.hpp>
#include <functional>
#include <memory>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// UI Refresh Scheduler //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Dispatch must queue asynchronously to one serialized UI context. A false
// result means no callback was queued. Render runs only on that context.
class UiRefreshScheduler {
public:
    using Flags = UiRefreshCoalescer::Flags;
    using Task = std::function<void()>;
    using Dispatch = std::function<bool(Task)>;
    using Render = std::function<bool(Flags)>;

    UiRefreshScheduler(Dispatch dispatch, Render render, Flags retryMask, util::LogSink log = {});
    ~UiRefreshScheduler();
    UiRefreshScheduler(UiRefreshScheduler const&) = delete;
    UiRefreshScheduler& operator=(UiRefreshScheduler const&) = delete;

    void Request(Flags flags);
    // Closes admission and disarms native work. Already admitted rendering may
    // finish; Stop never waits for the UI dispatcher or calls foreign code.
    void Stop() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
