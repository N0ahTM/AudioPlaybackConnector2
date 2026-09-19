#pragma once

#include <atomic>
#include <iostream>
#include <source_location>
#include <string_view>
#include <syncstream>

namespace {

// Each translation unit is one suite. Checks from its worker callbacks share an atomic counter;
// the runner reads the result only after that suite has drained its workers.
std::atomic<int> g_failures{0};

void Check(bool condition, std::string_view message, std::source_location location = std::source_location::current()) {
    if (condition) return;
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::osyncstream(std::cerr) << location.file_name() << ':' << location.line() << ": FAILED: " << message << '\n';
}

} // namespace
