#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Controlled Sink ///////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class BlockingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::binary_semaphore Entered{0};
    std::binary_semaphore Release{0};
    std::atomic_size_t Writes = 0;

private:
    void sink_it_(spdlog::details::log_msg const&) override {
        if (Writes.fetch_add(1) == 0) {
            Entered.release();
            Release.acquire();
        }
    }
    void flush_() override {}
};

struct ReleaseCompletion {
    std::binary_semaphore Entered{0};
    std::binary_semaphore Finished{0};
};

// The work context owns the pool until its worker has finished. Closing the
// current Windows work item defers its native release until this callback exits.
struct PoolRelease {
    std::shared_ptr<spdlog::details::thread_pool> Pool;
    std::shared_ptr<ReleaseCompletion> Completion;
    PTP_WORK Work = nullptr;

    ~PoolRelease() {
        Pool.reset();
        if (Work) CloseThreadpoolWork(Work);
    }

    static void CALLBACK Run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
        std::unique_ptr<PoolRelease> release(static_cast<PoolRelease*>(context));
        auto completion = release->Completion;
        completion->Entered.release();
        release.reset();
        completion->Finished.release();
    }
};

bool VerifyBoundedRelease() {
    auto sink = std::make_shared<BlockingSink>();
    auto pool = std::make_shared<spdlog::details::thread_pool>(8, 1);
    auto completion = std::make_shared<ReleaseCompletion>();
    auto release = std::make_unique<PoolRelease>();
    release->Completion = completion;
    release->Work = CreateThreadpoolWork(PoolRelease::Run, release.get(), nullptr);
    if (!release->Work) return false;

    auto logger = std::make_shared<spdlog::async_logger>(
        "bounded-release-probe", sink, pool, spdlog::async_overflow_policy::overrun_oldest);
    logger->info("block the worker");
    if (!sink->Entered.try_acquire_for(2s)) {
        sink->Release.release();
        return false;
    }
    logger->info("queued before shutdown");
    logger.reset();
    release->Pool = std::move(pool);
    // Ownership transfers before submission: the callback may run immediately.
    auto* context = release.release();
    SubmitThreadpoolWork(context->Work);
    const bool entered = completion->Entered.try_acquire_for(2s);
    const bool timedOut = !completion->Finished.try_acquire_for(20ms);
    // The caller can leave its bounded wait. Its stack is absent from the work
    // context; outstanding I/O and library state remain owned until completion.
    sink->Release.release();
    const bool finished = timedOut ? completion->Finished.try_acquire_for(2s) : true;
    return entered && timedOut && finished && sink->Writes == 2;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Rotation, Overflow and Shutdown ///////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    auto directory = std::filesystem::path(argv[1]) / L"Gr\u00fc\u00dfe-\u65e5\u672c-\U0001f3b5";
    std::filesystem::create_directories(directory);
    auto logPath = directory / "rotation.log";
    {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath.native(), 256, 2);
        spdlog::logger logger("rotation-probe", sink);
        logger.set_pattern("%v");
        for (int index = 0; index < 100; ++index)
            logger.info("record {}: bounded rotating file", index);
        logger.info("Unicode payload: Gr\u00fc\u00dfe \u65e5\u672c \U0001f3b5");
        logger.flush();
    }
    if (!std::filesystem::exists(directory / "rotation.1.log")) return 2;
    std::ifstream file(logPath, std::ios::binary);
    std::string content(std::istreambuf_iterator<char>{file}, {});
    if (content.find("Unicode payload: Gr\u00fc\u00dfe \u65e5\u672c \U0001f3b5") == std::string::npos) return 6;
    std::cout << "Unicode filename, rotation and UTF-8 payload passed\n";

    auto sink = std::make_shared<BlockingSink>();
    auto pool = std::make_shared<spdlog::details::thread_pool>(8, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        "private-pool-probe", sink, pool, spdlog::async_overflow_policy::overrun_oldest);
    logger->info("block the worker");
    if (!sink->Entered.try_acquire_for(2s)) {
        sink->Release.release();
        return 3;
    }
    for (int index = 0; index < 256; ++index)
        logger->info("record {}", index);
    auto const overruns = pool->overrun_counter();
    logger.reset();
    std::atomic_bool stopped = false;
    std::binary_semaphore shutdownEntered{0};
    std::jthread shutdown([pool = std::move(pool), &stopped, &shutdownEntered]() mutable {
        shutdownEntered.release();
        pool.reset();
        stopped = true;
    });
    shutdownEntered.acquire();
    std::this_thread::sleep_for(20ms);
    auto const blockedShutdown = !stopped.load();
    sink->Release.release();
    shutdown.join();
    if (overruns == 0 || !stopped || sink->Writes < 2) return 4;
    std::cout << "overruns=" << overruns << "; shutdown_waited_for_sink=" << blockedShutdown << '\n';
    for (int iteration = 0; iteration < 20; ++iteration)
        if (!VerifyBoundedRelease()) return 5;
    std::cout << "bounded caller wait and owned late pool release passed 20 iterations\n";
}
