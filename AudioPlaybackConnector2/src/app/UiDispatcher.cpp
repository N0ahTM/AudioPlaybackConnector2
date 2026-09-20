#include <app/UiDispatcher.hpp>
#include <atomic>
#include <condition_variable>
#include <vector>
#include <mutex>
#include <stdexcept>
#include <utility>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Dispatch State ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// In-flight Run calls retain state through native posting. Queued callbacks
// hold it weakly and never capture the facade; Stop closes all later admission.
struct UiDispatcher::State {
    struct PendingTask {
        Task Work;
        bool Claimed = false;
    };
    static constexpr UINT Message = WM_APP + 2;
    State(HWND window, Dispatch dispatch, util::LogSink log)
        : Window(window), UiThread(GetCurrentThreadId()), Enqueue(std::move(dispatch)), Log(std::move(log)) {}

    bool Invoke(Task& work) noexcept {
        if (Stopped.load()) return false;
        try {
            work();
            return true;
        } catch (...) {
            Log.UnknownException(L"[UiDispatcher] UI work failed");
            return false;
        }
    }

    bool Fallback(Task work) {
        auto pending = std::make_shared<PendingTask>(std::move(work));
        {
            std::scoped_lock lock(Mutex);
            if (Stopped.load()) return false;
            Queue.push_back(pending);
            ++Posting;
        }
        // One notification per post avoids publishing acceptance before a shared
        // wakeup has succeeded. Posting does not invoke the window procedure.
        const bool posted = PostMessageW(Window, Message, 0, 0) != FALSE;
        bool accepted;
        {
            std::scoped_lock lock(Mutex);
            accepted = posted || pending->Claimed;
            if (!accepted) std::erase(Queue, pending);
            --Posting;
        }
        PostsFinished.notify_all();
        if (!accepted) Log.Trace(L"[UiDispatcher] Both UI transports rejected work");
        // The local reference keeps a failed task's destructor outside Mutex.
        return accepted;
    }

    void Drain() noexcept {
        std::vector<std::shared_ptr<PendingTask>> ready;
        {
            std::scoped_lock lock(Mutex);
            ready.swap(Queue);
            for (auto const& task : ready)
                task->Claimed = true;
        }
        for (auto const& task : ready)
            static_cast<void>(Invoke(task->Work));
    }

    void Stop() noexcept {
        std::vector<std::shared_ptr<PendingTask>> abandoned;
        {
            std::unique_lock lock(Mutex);
            Stopped.store(true);
            abandoned.swap(Queue);
            // PostMessage is nonblocking and needs no UI progress. Once its
            // admitted calls finish, the host may safely destroy the HWND.
            PostsFinished.wait(lock, [&] { return Posting == 0; });
        }
    }

    const HWND Window;
    const DWORD UiThread;
    Dispatch Enqueue;
    util::LogSink Log;
    std::atomic_bool Stopped = false;
    // Queue membership, claims and native posts only; no callbacks/Win32 under this lock.
    std::mutex Mutex;
    std::condition_variable PostsFinished;
    std::size_t Posting = 0;
    std::vector<std::shared_ptr<PendingTask>> Queue;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

UiDispatcher::UiDispatcher(HWND window, Dispatch dispatch, util::LogSink log) {
    if (!window || !dispatch || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) {
        throw std::invalid_argument("UiDispatcher requires its UI window and an async dispatcher");
    }
    m_state = std::make_shared<State>(window, std::move(dispatch), std::move(log));
}

UiDispatcher::~UiDispatcher() {
    Stop();
}

bool UiDispatcher::Run(Task task) noexcept {
    auto state = m_state;
    try {
        if (!task || state->Stopped.load()) return false;
        if (GetCurrentThreadId() == state->UiThread) return state->Invoke(task);
        try {
            if (state->Enqueue([weak = std::weak_ptr<State>(state), work = task]() mutable {
                    if (auto current = weak.lock()) static_cast<void>(current->Invoke(work));
                }))
                return true;
        } catch (...) {
            state->Log.UnknownException(L"[UiDispatcher] Dispatcher rejected work; using window queue");
        }
        return state->Fallback(std::move(task));
    } catch (...) {
        state->Log.UnknownException(L"[UiDispatcher] Work could not be queued");
        return false;
    }
}

bool UiDispatcher::HandleMessage(UINT message) noexcept {
    if (message != State::Message) return false;
    auto state = m_state;
    state->Drain();
    return true;
}

void UiDispatcher::Stop() noexcept {
    auto state = m_state;
    state->Stop();
}
