#include "TestCheck.hpp"

#include <util/Logger.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <winioctl.h>
#include <wil/resource.h>

namespace {
using namespace std::chrono_literals;

std::string Read(std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, {}};
}

std::size_t Count(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Logger Ownership and File Contract ////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

int RunLoggerTests() {
    auto directory = std::filesystem::temp_directory_path() /
                     std::format(L"APC-Logger-{}-{}", GetCurrentProcessId(), GetTickCount64()) /
                     L"Gr\u00fc\u00dfe-\u65e5\u672c-\U0001f3b5";
    std::filesystem::create_directories(directory);
    {
        auto path = directory / L"unicode.log";
        util::LogSink late;
        {
            util::Logger logger(path);
            late = logger.Sink();
            Check(late.Path() == path, "log handle reports its owner's actual Unicode path");
            late.Write(L"Gr\u00fc\u00dfe \u65e5\u672c \U0001f3b5");
            late.RequestFlush();
            Check(logger.Shutdown(5s), "shutdown drains and flushes the Unicode record");
            auto before = Read(path);
            Check(before.find("Gr\xC3\xBC\xC3\x9F"
                              "e \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5") != std::string::npos,
                  "native UTF-16 path contains the exact UTF-8 payload");
            late.Write(L"must not be written after shutdown");
            late.RequestFlush();
            Check(logger.Shutdown(5s), "repeated shutdown observes the same completion");
            Check(late.Path() == path, "path remains available while the stopped owner lives");
            Check(Read(path) == before, "closed logger rejects late writes and flushes");
            Check(logger.Statistics().Errors == 0, "valid Unicode logging has no errors");
        }
        auto before = Read(path);
        late.Write(L"owner already destroyed");
        Check(Read(path) == before, "non-owning sink is inert after owner destruction");
    }
    {
        auto path = directory / L"overflow.log";
        util::Logger logger(path, 2, 16 * 1024 * 1024);
        auto sink = logger.Sink();
        constexpr std::size_t producers = 4;
        constexpr std::size_t perProducer = 5000;
        std::barrier start(static_cast<std::ptrdiff_t>(producers));
        std::vector<std::jthread> threads;
        for (std::size_t index = 0; index < producers; ++index) {
            threads.emplace_back([&] {
                start.arrive_and_wait();
                for (std::size_t record = 0; record < perProducer; ++record)
                    sink.Write(L"[record]");
            });
        }
        threads.clear();
        Check(logger.Shutdown(5s), "parallel producers drain before pool destruction");
        auto statistics = logger.Statistics();
        auto output = Read(path);
        Check(Count(output, "[record]") + statistics.Dropped == producers * perProducer,
              "every admitted record is either persisted or counted as dropped");
        if (statistics.Dropped != 0)
            Check(output.find(std::format("Dropped {} queued", statistics.Dropped)) != std::string::npos,
                  "drop notice survives queue drain and cannot itself overflow");
        Check(statistics.Errors == 0, "parallel logging has no sink errors");
    }
    {
        util::Logger logger(directory / L"shutdown.log", 16);
        auto sink = logger.Sink();
        std::barrier start(5);
        std::atomic_bool stop = false;
        std::vector<std::jthread> producers;
        for (int index = 0; index < 4; ++index) {
            producers.emplace_back([&] {
                start.arrive_and_wait();
                while (!stop.load())
                    sink.Write(L"racing shutdown");
            });
        }
        start.arrive_and_wait();
        std::array<bool, 2> finished{};
        std::jthread first([&] { finished[0] = logger.Shutdown(5s); });
        std::jthread second([&] { finished[1] = logger.Shutdown(5s); });
        first.join();
        second.join();
        stop = true;
        producers.clear();
        Check(finished[0] && finished[1], "concurrent shutdown shares one cleanup during producer activity");
        auto before = Read(directory / L"shutdown.log");
        sink.Write(L"late record");
        Check(Read(directory / L"shutdown.log") == before, "shutdown completion freezes output");
    }
    {
        util::Logger logger(directory / L"rotation.log", 1000, 256);
        auto sink = logger.Sink();
        for (int index = 0; index < 100; ++index)
            sink.Write(L"rotation record");
        Check(logger.Shutdown(5s), "rotation drains successfully");
        Check(std::filesystem::exists(directory / L"rotation.1.log"), "one rotated backup is retained");
        Check(!std::filesystem::exists(directory / L"rotation.2.log"), "rotation does not retain extra backups");
    }
    {
        const auto path = directory / L"emergency.log";
        util::EmergencyLog emergency;
        {
            util::Logger logger(path, 1000);
            emergency = logger.Emergency();
            for (int index = 0; index < 125; ++index)
                logger.Sink().Write(std::format(L"tail-record-{:03}", index));
            Check(logger.Shutdown(5s), "normal worker stops before independent emergency output");
        }
        Check(emergency.Dump(L"after-owner-destruction", 0x1234), "emergency tail survives without the logger worker");
        auto output = Read(path);
        auto begin = output.find("[Logger] BEGIN diagnostic tail");
        Check(begin != std::string::npos, "emergency output has an explicit boundary");
        if (begin != std::string::npos) {
            auto tail = std::string_view(output).substr(begin);
            Check(Count(tail, "tail-record-") == 100, "emergency tail retains exactly its fixed capacity");
            Check(tail.find("tail-record-024") == std::string_view::npos, "oldest records leave the ring");
            Check(tail.find("tail-record-025") < tail.find("tail-record-124"), "tail order remains chronological");
            Check(tail.find("exception=0x00001234") != std::string_view::npos,
                  "exception code survives emergency output");
        }
        Check(emergency.Write("emergency-only record\r\n"), "direct emergency output needs no asynchronous worker");
        Check(Read(path).ends_with("emergency-only record\r\n"), "direct emergency bytes are written unchanged");
    }
    {
        const auto path = directory / L"emergency-unicode.log";
        util::Logger logger(path);
        logger.Sink().Write(std::wstring(2000, L'\u65e5'));
        Check(logger.Shutdown(5s), "long Unicode record drains");
        Check(logger.Emergency().Dump(L"bounded Unicode"), "bounded Unicode tail dumps");
        auto output = Read(path);
        auto begin = output.find("[Logger] BEGIN diagnostic tail");
        Check(begin != std::string::npos, "Unicode tail boundary exists");
        if (begin != std::string::npos) {
            auto tail = std::string_view(output).substr(begin);
            Check(tail.size() < 1300, "one long record cannot grow the fixed emergency tail");
            Check(tail.find("...\r\n") != std::string_view::npos, "truncation is visible");
            Check(!util::Utf8ToUtf16(tail).empty(), "truncation preserves complete UTF-8 characters");
        }
    }
    {
        // A nonempty directory at the backup filename deterministically prevents
        // rotation. This exercises the real library sink's error handler.
        auto backup = directory / L"failure.1.log";
        std::filesystem::create_directory(backup);
        std::ofstream(backup / L"keep.txt") << "occupied";
        util::Logger logger(directory / L"failure.log", 1000, 256);
        for (int index = 0; index < 20; ++index)
            logger.Sink().Write(L"rotation must fail here");
        Check(logger.Shutdown(5s), "sink failure still completes owned cleanup");
        Check(logger.Statistics().Errors > 0, "sink failures are observable without recursive logging");
    }
    for (bool destroyOwner : {false, true}) {
        // Hold an actual filesystem oplock on the rotation destination. The
        // worker's incompatible delete waits for acknowledgement; no fake sink
        // or production testing hook changes the logger implementation.
        auto path = directory / (destroyOwner ? L"destroyed.log" : L"blocked.log");
        auto backup = directory / (destroyOwner ? L"destroyed.1.log" : L"blocked.1.log");
        auto logger = std::make_unique<util::Logger>(path, 1000, 256);
        auto late = logger->Sink();
        std::ofstream(backup) << "oplock target";
        wil::unique_hfile blocker(CreateFileW(backup.c_str(),
                                              GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_FLAG_OVERLAPPED,
                                              nullptr));
        wil::unique_handle broken(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Check(blocker && broken, "open oplock target and completion event");
        if (blocker && broken) {
            OVERLAPPED operation{};
            operation.hEvent = broken.get();
            DWORD bytes = 0;
            const auto granted = DeviceIoControl(
                blocker.get(), FSCTL_REQUEST_OPLOCK_LEVEL_1, nullptr, 0, nullptr, 0, &bytes, &operation);
            const auto pending = !granted && GetLastError() == ERROR_IO_PENDING;
            Check(pending, "filesystem grants exclusive asynchronous oplock");
            if (pending) {
                auto cancel = wil::scope_exit([&] {
                    if (blocker) {
                        CancelIoEx(blocker.get(), &operation);
                        GetOverlappedResult(blocker.get(), &operation, &bytes, TRUE);
                    }
                });
                for (int index = 0; index < 20; ++index)
                    late.Write(L"blocked rotation record");
                const auto observed = WaitForSingleObject(broken.get(), 5000) == WAIT_OBJECT_0;
                Check(observed, "production rotation reaches the blocking filesystem operation");
                if (observed) {
                    const auto start = std::chrono::steady_clock::now();
                    Check(!logger->Shutdown(20ms), "blocked production I/O reports incomplete bounded shutdown");
                    Check(std::chrono::steady_clock::now() - start < 1s,
                          "shutdown caller does not join blocked worker");
                    late.Write(L"rejected after blocked shutdown");
                    if (destroyOwner) {
                        const auto destroyStart = std::chrono::steady_clock::now();
                        logger.reset();
                        Check(std::chrono::steady_clock::now() - destroyStart < 1s,
                              "owner destructor does not join blocked filesystem I/O");
                        late.Write(L"rejected after owner destruction");
                    }
                    GetOverlappedResult(blocker.get(), &operation, &bytes, TRUE);
                    blocker.reset(); // Acknowledge break and release the worker.
                    if (logger) {
                        Check(logger->Shutdown(5s), "same cleanup completes when real filesystem I/O resumes");
                        Check(logger->Statistics().Errors == 0, "blocking and resuming causes no sink errors");
                    } else {
                        const auto deadline = std::chrono::steady_clock::now() + 5s;
                        while (!late.Path().empty() && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(1ms);
                        Check(late.Path().empty(), "deferred cleanup releases its final state after owner destruction");
                    }
                    Check(Read(path).find("rejected after") == std::string::npos,
                          "closed admission rejects late record");
                }
            }
        }
    }
    return g_failures.load();
}
