#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <semaphore>
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
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Rotation, Overflow and Shutdown ///////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    auto logPath = directory / "rotation.log";
    {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath.string(), 256, 2);
        spdlog::logger logger("rotation-probe", sink);
        logger.set_pattern("%v");
        for (int index = 0; index < 100; ++index)
            logger.info("record {}: bounded rotating file", index);
        logger.flush();
    }
    if (!std::filesystem::exists(directory / "rotation.1.log")) return 2;

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
}
